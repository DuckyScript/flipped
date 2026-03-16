#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_version.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <gui/modules/text_input.h>
#include <storage/storage.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>

#define TAG "Terminal"

#define CMD_BUF_SIZE 128
#define OUTPUT_BUF_SIZE 4096
#define DIR_NAME_MAX 256
#define WIFI_MAX_URL_LEN 512

// Package manager paths
#define PKG_INSTALL_PATH EXT_PATH("app_install")
#define PKG_MANIFEST PKG_INSTALL_PATH "/manifest.txt"
#define PKG_APPS_DIR EXT_PATH("apps")
#define PKG_MAX_APPS 32
#define PKG_MAX_NAME 64
#define PKG_MAX_FILE 128
#define PKG_MAX_CAT 32
#define PKG_MAX_DESC 128
#define PKG_MANIFEST_LINE 384

// WiFi UART settings (shared protocol with App Store)
#define WIFI_UART_BAUD 115200
#define WIFI_PING_TIMEOUT_MS 1000
#define WIFI_CMD_TIMEOUT_MS 5000
#define WIFI_RX_BUF_SIZE 1024

typedef enum {
    TerminalViewHome,
    TerminalViewOutput,
    TerminalViewInput,
} TerminalView;

enum {
    TerminalMenuEnterCmd = 0,
    TerminalMenuViewOutput,
    TerminalMenuClear,
};

typedef struct {
    FuriHalSerialHandle* serial;
    FuriStreamBuffer* rx_stream;
    bool detected;
    bool connected;
} WifiState;

typedef struct {
    char name[PKG_MAX_NAME];
    char filename[PKG_MAX_FILE];
    char category[PKG_MAX_CAT];
    char description[PKG_MAX_DESC];
} PkgEntry;

typedef struct {
    PkgEntry apps[PKG_MAX_APPS];
    uint32_t count;
    bool loaded;
    char repo_url[WIFI_MAX_URL_LEN];
} PkgManager;

typedef struct {
    Gui* gui;
    Storage* storage;
    NotificationApp* notifications;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    TextBox* text_box;
    TextInput* text_input;

    FuriString* output;
    char cmd_buf[CMD_BUF_SIZE];
    char cwd[DIR_NAME_MAX];

    WifiState wifi;
    PkgManager pkg;
} TerminalApp;

// --- WiFi UART ---

static void terminal_wifi_rx_cb(
    FuriHalSerialHandle* handle,
    FuriHalSerialRxEvent event,
    void* context) {
    UNUSED(handle);
    WifiState* wifi = context;
    if(event == FuriHalSerialRxEventData) {
        uint8_t data = furi_hal_serial_async_rx(handle);
        furi_stream_buffer_send(wifi->rx_stream, &data, 1, 0);
    }
}

static void wifi_flush(WifiState* wifi) {
    uint8_t tmp;
    while(furi_stream_buffer_receive(wifi->rx_stream, &tmp, 1, 0) > 0) {
    }
}

static void wifi_send(WifiState* wifi, const char* cmd) {
    furi_hal_serial_tx(wifi->serial, (const uint8_t*)cmd, strlen(cmd));
    furi_hal_serial_tx_wait_complete(wifi->serial);
}

static size_t wifi_read_line(WifiState* wifi, char* buf, size_t len, uint32_t timeout_ms) {
    uint32_t start = furi_get_tick();
    size_t pos = 0;
    while(pos < len - 1) {
        uint32_t elapsed =
            (furi_get_tick() - start) * 1000 / furi_kernel_get_tick_frequency();
        if(elapsed >= timeout_ms) break;
        uint8_t ch;
        if(furi_stream_buffer_receive(wifi->rx_stream, &ch, 1, 10) == 0) continue;
        if(ch == '\n') {
            buf[pos] = '\0';
            if(pos > 0 && buf[pos - 1] == '\r') buf[--pos] = '\0';
            return pos;
        }
        buf[pos++] = (char)ch;
    }
    buf[pos] = '\0';
    return pos;
}

static bool wifi_init(WifiState* wifi) {
    wifi->rx_stream = furi_stream_buffer_alloc(WIFI_RX_BUF_SIZE, 1);
    wifi->detected = false;
    wifi->connected = false;

    wifi->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(!wifi->serial) {
        wifi->serial = furi_hal_serial_control_acquire(FuriHalSerialIdLpuart);
    }
    if(!wifi->serial) {
        furi_stream_buffer_free(wifi->rx_stream);
        wifi->rx_stream = NULL;
        return false;
    }

    furi_hal_serial_init(wifi->serial, WIFI_UART_BAUD);
    furi_hal_serial_async_rx_start(wifi->serial, terminal_wifi_rx_cb, wifi, false);
    return true;
}

static void wifi_deinit(WifiState* wifi) {
    if(wifi->serial) {
        furi_hal_serial_async_rx_stop(wifi->serial);
        furi_hal_serial_deinit(wifi->serial);
        furi_hal_serial_control_release(wifi->serial);
        wifi->serial = NULL;
    }
    if(wifi->rx_stream) {
        furi_stream_buffer_free(wifi->rx_stream);
        wifi->rx_stream = NULL;
    }
}

static bool wifi_detect(WifiState* wifi) {
    if(!wifi->serial) return false;
    wifi_flush(wifi);
    wifi_send(wifi, "AT\r\n");
    char resp[64];
    size_t len = wifi_read_line(wifi, resp, sizeof(resp), WIFI_PING_TIMEOUT_MS);
    if(len > 0 && strstr(resp, "OK")) {
        wifi->detected = true;
        wifi_flush(wifi);
        wifi_send(wifi, "WIFI_STATUS\r\n");
        len = wifi_read_line(wifi, resp, sizeof(resp), WIFI_PING_TIMEOUT_MS);
        wifi->connected = (len > 0 && strstr(resp, "CONNECTED"));
        return true;
    }
    wifi->detected = false;
    return false;
}

// --- Output ---

static void terminal_update_textbox(TerminalApp* app) {
    text_box_set_text(app->text_box, furi_string_get_cstr(app->output));
    text_box_set_focus(app->text_box, TextBoxFocusEnd);
}

static void terminal_print(TerminalApp* app, const char* text) {
    furi_string_cat_str(app->output, text);
    if(furi_string_size(app->output) > OUTPUT_BUF_SIZE - 256) {
        size_t cut = furi_string_size(app->output) - (OUTPUT_BUF_SIZE / 2);
        size_t nl = furi_string_search_char(app->output, '\n', cut);
        if(nl != FURI_STRING_FAILURE) {
            furi_string_right(app->output, nl + 1);
        }
    }
}

static void terminal_println(TerminalApp* app, const char* text) {
    terminal_print(app, text);
    terminal_print(app, "\n");
}

static void terminal_printf(TerminalApp* app, const char* fmt, ...) {
    FuriString* tmp = furi_string_alloc();
    va_list args;
    va_start(args, fmt);
    furi_string_vprintf(tmp, fmt, args);
    va_end(args);
    terminal_print(app, furi_string_get_cstr(tmp));
    furi_string_free(tmp);
}

// --- Path Resolution ---

static void resolve_path(TerminalApp* app, const char* input, char* out, size_t out_size) {
    if(input[0] == '/') {
        strlcpy(out, input, out_size);
    } else {
        snprintf(out, out_size, "%s/%s", app->cwd, input);
    }
}

// --- Commands ---

static void cmd_help(TerminalApp* app) {
    terminal_println(app, "Commands:");
    terminal_println(app, " help       - show this help");
    terminal_println(app, " ls [path]  - list directory");
    terminal_println(app, " cat <file> - read file");
    terminal_println(app, " rm <path>  - delete");
    terminal_println(app, " mkdir <d>  - create dir");
    terminal_println(app, " mv <s> <d> - move/rename");
    terminal_println(app, " cp <s> <d> - copy");
    terminal_println(app, " stat <p>   - file info");
    terminal_println(app, " df         - disk space");
    terminal_println(app, " pwd        - working dir");
    terminal_println(app, " cd <path>  - change dir");
    terminal_println(app, " clear      - clear output");
    terminal_println(app, " info       - device info");
    terminal_println(app, " ping       - detect WiFi");
    terminal_println(app, " wifi       - WiFi status");
    terminal_println(app, " ssid <n>   - set WiFi SSID");
    terminal_println(app, " pass <p>   - set WiFi pass");
    terminal_println(app, " connect    - join WiFi");
    terminal_println(app, " wget <url> <file>");
    terminal_println(app, " pkg         - package manager");
}

static void cmd_ls(TerminalApp* app, const char* path) {
    char resolved[DIR_NAME_MAX];
    if(path && strlen(path) > 0) {
        resolve_path(app, path, resolved, sizeof(resolved));
    } else {
        strlcpy(resolved, app->cwd, sizeof(resolved));
    }

    File* dir = storage_file_alloc(app->storage);
    if(!storage_dir_open(dir, resolved)) {
        terminal_printf(app, "ls: cannot open %s\n", resolved);
        storage_dir_close(dir);
        storage_file_free(dir);
        return;
    }

    FileInfo finfo;
    char name[DIR_NAME_MAX];
    uint32_t count = 0;

    while(storage_dir_read(dir, &finfo, name, sizeof(name))) {
        if(file_info_is_dir(&finfo)) {
            terminal_printf(app, " [DIR] %s\n", name);
        } else {
            if(finfo.size >= 1024) {
                terminal_printf(app, " %5lluK %s\n", finfo.size / 1024, name);
            } else {
                terminal_printf(app, " %5llu  %s\n", finfo.size, name);
            }
        }
        count++;
    }

    if(count == 0) {
        terminal_println(app, "(empty)");
    } else {
        terminal_printf(app, "%lu items\n", count);
    }

    storage_dir_close(dir);
    storage_file_free(dir);
}

static void cmd_cat(TerminalApp* app, const char* path) {
    if(!path || strlen(path) == 0) {
        terminal_println(app, "usage: cat <file>");
        return;
    }

    char resolved[DIR_NAME_MAX];
    resolve_path(app, path, resolved, sizeof(resolved));

    File* file = storage_file_alloc(app->storage);
    if(!storage_file_open(file, resolved, FSAM_READ, FSOM_OPEN_EXISTING)) {
        terminal_printf(app, "cat: cannot open %s\n", resolved);
        storage_file_free(file);
        return;
    }

    char buf[129];
    size_t total = 0;
    const size_t max_read = 2048;

    while(total < max_read) {
        size_t to_read = sizeof(buf) - 1;
        if(total + to_read > max_read) to_read = max_read - total;
        size_t bytes = storage_file_read(file, buf, to_read);
        if(bytes == 0) break;
        buf[bytes] = '\0';
        terminal_print(app, buf);
        total += bytes;
    }

    if(total >= max_read) {
        terminal_println(app, "\n... (truncated)");
    } else {
        terminal_print(app, "\n");
    }

    storage_file_close(file);
    storage_file_free(file);
}

static void cmd_rm(TerminalApp* app, const char* path) {
    if(!path || strlen(path) == 0) {
        terminal_println(app, "usage: rm <path>");
        return;
    }
    char resolved[DIR_NAME_MAX];
    resolve_path(app, path, resolved, sizeof(resolved));
    if(storage_simply_remove(app->storage, resolved)) {
        terminal_printf(app, "removed: %s\n", resolved);
    } else {
        terminal_printf(app, "rm: failed %s\n", resolved);
    }
}

static void cmd_mkdir(TerminalApp* app, const char* path) {
    if(!path || strlen(path) == 0) {
        terminal_println(app, "usage: mkdir <dir>");
        return;
    }
    char resolved[DIR_NAME_MAX];
    resolve_path(app, path, resolved, sizeof(resolved));
    if(storage_simply_mkdir(app->storage, resolved)) {
        terminal_printf(app, "created: %s\n", resolved);
    } else {
        terminal_printf(app, "mkdir: failed %s\n", resolved);
    }
}

static void cmd_stat(TerminalApp* app, const char* path) {
    if(!path || strlen(path) == 0) {
        terminal_println(app, "usage: stat <path>");
        return;
    }
    char resolved[DIR_NAME_MAX];
    resolve_path(app, path, resolved, sizeof(resolved));
    FileInfo finfo;
    if(storage_common_stat(app->storage, resolved, &finfo) != FSE_OK) {
        terminal_printf(app, "stat: not found %s\n", resolved);
        return;
    }
    if(file_info_is_dir(&finfo)) {
        terminal_printf(app, "%s: directory\n", resolved);
    } else {
        terminal_printf(app, "%s: %llu bytes\n", resolved, finfo.size);
    }
}

static void cmd_mv(TerminalApp* app, const char* args) {
    if(!args || strlen(args) == 0) {
        terminal_println(app, "usage: mv <src> <dst>");
        return;
    }
    const char* space = strchr(args, ' ');
    if(!space) {
        terminal_println(app, "usage: mv <src> <dst>");
        return;
    }
    char src_raw[DIR_NAME_MAX], dst_raw[DIR_NAME_MAX];
    size_t src_len = space - args;
    if(src_len >= sizeof(src_raw)) src_len = sizeof(src_raw) - 1;
    memcpy(src_raw, args, src_len);
    src_raw[src_len] = '\0';
    strlcpy(dst_raw, space + 1, sizeof(dst_raw));

    char src[DIR_NAME_MAX], dst[DIR_NAME_MAX];
    resolve_path(app, src_raw, src, sizeof(src));
    resolve_path(app, dst_raw, dst, sizeof(dst));

    FS_Error err = storage_common_rename(app->storage, src, dst);
    if(err == FSE_OK) {
        terminal_printf(app, "%s -> %s\n", src, dst);
    } else {
        terminal_printf(app, "mv: failed (err %d)\n", err);
    }
}

static void cmd_cp(TerminalApp* app, const char* args) {
    if(!args || strlen(args) == 0) {
        terminal_println(app, "usage: cp <src> <dst>");
        return;
    }
    const char* space = strchr(args, ' ');
    if(!space) {
        terminal_println(app, "usage: cp <src> <dst>");
        return;
    }
    char src_raw[DIR_NAME_MAX], dst_raw[DIR_NAME_MAX];
    size_t src_len = space - args;
    if(src_len >= sizeof(src_raw)) src_len = sizeof(src_raw) - 1;
    memcpy(src_raw, args, src_len);
    src_raw[src_len] = '\0';
    strlcpy(dst_raw, space + 1, sizeof(dst_raw));

    char src[DIR_NAME_MAX], dst[DIR_NAME_MAX];
    resolve_path(app, src_raw, src, sizeof(src));
    resolve_path(app, dst_raw, dst, sizeof(dst));

    FS_Error err = storage_common_copy(app->storage, src, dst);
    if(err == FSE_OK) {
        terminal_printf(app, "copied %s -> %s\n", src, dst);
    } else {
        terminal_printf(app, "cp: failed (err %d)\n", err);
    }
}

static void cmd_df(TerminalApp* app) {
    uint64_t total, free_space;
    FS_Error err = storage_common_fs_info(app->storage, "/ext", &total, &free_space);
    if(err == FSE_OK) {
        terminal_printf(
            app,
            "SD Card:\n Total: %llu KB\n Free:  %llu KB\n Used:  %llu KB\n",
            total / 1024,
            free_space / 1024,
            (total - free_space) / 1024);
    } else {
        terminal_println(app, "df: cannot read SD info");
    }
}

static void cmd_cd(TerminalApp* app, const char* path) {
    if(!path || strlen(path) == 0 || strcmp(path, "/") == 0) {
        strlcpy(app->cwd, "/ext", sizeof(app->cwd));
        terminal_printf(app, "%s\n", app->cwd);
        return;
    }
    if(strcmp(path, "..") == 0) {
        char* last_slash = strrchr(app->cwd, '/');
        if(last_slash && last_slash != app->cwd) {
            *last_slash = '\0';
        }
        terminal_printf(app, "%s\n", app->cwd);
        return;
    }
    char resolved[DIR_NAME_MAX];
    resolve_path(app, path, resolved, sizeof(resolved));
    FileInfo finfo;
    if(storage_common_stat(app->storage, resolved, &finfo) == FSE_OK &&
       file_info_is_dir(&finfo)) {
        strlcpy(app->cwd, resolved, sizeof(app->cwd));
        terminal_printf(app, "%s\n", app->cwd);
    } else {
        terminal_printf(app, "cd: not a directory: %s\n", resolved);
    }
}

static void cmd_info(TerminalApp* app) {
    terminal_println(app, "Flipper Zero");
    uint16_t major, minor;
    furi_hal_info_get_api_version(&major, &minor);
    terminal_printf(app, "API: %u.%u\n", major, minor);
    const char* name = furi_hal_version_get_name_ptr();
    if(name) {
        terminal_printf(app, "Name: %s\n", name);
    }
    terminal_printf(
        app,
        "HW: %d.F%dB%d.%d\n",
        furi_hal_version_get_hw_version(),
        furi_hal_version_get_hw_target(),
        furi_hal_version_get_hw_body(),
        furi_hal_version_get_hw_connect());
}

// --- WiFi Commands ---

static void cmd_ping(TerminalApp* app) {
    if(!app->wifi.serial) {
        terminal_println(app, "No serial port available");
        return;
    }
    if(wifi_detect(&app->wifi)) {
        if(app->wifi.connected) {
            terminal_println(app, "WiFi module: OK (connected)");
        } else {
            terminal_println(app, "WiFi module: OK (no network)");
        }
    } else {
        terminal_println(app, "WiFi module: not found");
    }
}

static void cmd_wifi_status(TerminalApp* app) {
    if(!app->wifi.serial || !app->wifi.detected) {
        terminal_println(app, "No WiFi module. Use 'ping'");
        return;
    }
    wifi_flush(&app->wifi);
    wifi_send(&app->wifi, "WIFI_STATUS\r\n");
    char resp[64];
    wifi_read_line(&app->wifi, resp, sizeof(resp), WIFI_CMD_TIMEOUT_MS);
    terminal_printf(app, "WiFi: %s\n", resp);
}

static void cmd_wifi_ssid(TerminalApp* app, const char* ssid) {
    if(!app->wifi.serial || !app->wifi.detected) {
        terminal_println(app, "No WiFi module. Use 'ping'");
        return;
    }
    if(!ssid || strlen(ssid) == 0) {
        terminal_println(app, "usage: ssid <network_name>");
        return;
    }
    wifi_flush(&app->wifi);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "WIFI_SSID %s\r\n", ssid);
    wifi_send(&app->wifi, cmd);
    char resp[64];
    wifi_read_line(&app->wifi, resp, sizeof(resp), WIFI_CMD_TIMEOUT_MS);
    terminal_printf(app, "SSID set: %s\n", resp);
}

static void cmd_wifi_pass(TerminalApp* app, const char* pass) {
    if(!app->wifi.serial || !app->wifi.detected) {
        terminal_println(app, "No WiFi module. Use 'ping'");
        return;
    }
    if(!pass || strlen(pass) == 0) {
        terminal_println(app, "usage: pass <password>");
        return;
    }
    wifi_flush(&app->wifi);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "WIFI_PASS %s\r\n", pass);
    wifi_send(&app->wifi, cmd);
    char resp[64];
    wifi_read_line(&app->wifi, resp, sizeof(resp), WIFI_CMD_TIMEOUT_MS);
    terminal_println(app, "Password set");
}

static void cmd_wifi_connect(TerminalApp* app) {
    if(!app->wifi.serial || !app->wifi.detected) {
        terminal_println(app, "No WiFi module. Use 'ping'");
        return;
    }
    terminal_println(app, "Connecting...");
    wifi_flush(&app->wifi);
    wifi_send(&app->wifi, "WIFI_CONNECT\r\n");
    char resp[64];
    wifi_read_line(&app->wifi, resp, sizeof(resp), 20000);
    if(strstr(resp, "CONNECTED")) {
        app->wifi.connected = true;
        terminal_println(app, "Connected!");
    } else {
        terminal_printf(app, "Failed: %s\n", resp);
    }
}

static void cmd_wget(TerminalApp* app, const char* args) {
    if(!app->wifi.serial || !app->wifi.detected) {
        terminal_println(app, "No WiFi module. Use 'ping'");
        return;
    }
    if(!app->wifi.connected) {
        terminal_println(app, "WiFi not connected");
        return;
    }
    if(!args || strlen(args) == 0) {
        terminal_println(app, "usage: wget <url> <file>");
        return;
    }

    const char* space = strchr(args, ' ');
    if(!space) {
        terminal_println(app, "usage: wget <url> <file>");
        return;
    }

    char url[512];
    size_t url_len = space - args;
    if(url_len >= sizeof(url)) url_len = sizeof(url) - 1;
    memcpy(url, args, url_len);
    url[url_len] = '\0';

    char dest_raw[DIR_NAME_MAX];
    strlcpy(dest_raw, space + 1, sizeof(dest_raw));
    char dest[DIR_NAME_MAX];
    resolve_path(app, dest_raw, dest, sizeof(dest));

    terminal_printf(app, "Downloading to %s...\n", dest);

    wifi_flush(&app->wifi);
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "DOWNLOAD %s\r\n", url);
    wifi_send(&app->wifi, cmd);

    char resp[128];
    size_t len = wifi_read_line(&app->wifi, resp, sizeof(resp), WIFI_CMD_TIMEOUT_MS);

    if(len == 0) {
        terminal_println(app, "No response from module");
        return;
    }
    if(strncmp(resp, "ERROR:", 6) == 0) {
        terminal_printf(app, "Error: %s\n", resp + 6);
        return;
    }
    if(strncmp(resp, "SIZE:", 5) != 0) {
        terminal_printf(app, "Bad response: %s\n", resp);
        return;
    }

    uint32_t file_size = strtoul(resp + 5, NULL, 10);
    if(file_size == 0 || file_size > 1024 * 1024) {
        terminal_printf(app, "Bad size: %lu\n", file_size);
        return;
    }

    terminal_printf(app, "Size: %lu bytes\n", file_size);

    File* file = storage_file_alloc(app->storage);
    if(!storage_file_open(file, dest, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        terminal_printf(app, "Cannot create %s\n", dest);
        storage_file_free(file);
        return;
    }

    uint32_t remaining = file_size;
    bool ok = true;

    while(remaining > 0) {
        uint8_t chunk[512];
        size_t to_read = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
        uint32_t start = furi_get_tick();
        size_t total_chunk = 0;

        while(total_chunk < to_read) {
            uint32_t elapsed =
                (furi_get_tick() - start) * 1000 / furi_kernel_get_tick_frequency();
            if(elapsed >= 30000) {
                ok = false;
                break;
            }
            size_t got = furi_stream_buffer_receive(
                app->wifi.rx_stream, chunk + total_chunk, to_read - total_chunk, 50);
            total_chunk += got;
        }
        if(!ok) break;

        if(storage_file_write(file, chunk, total_chunk) != total_chunk) {
            ok = false;
            break;
        }
        remaining -= total_chunk;
    }

    storage_file_close(file);
    storage_file_free(file);

    if(ok) {
        wifi_read_line(&app->wifi, resp, sizeof(resp), 2000);
        terminal_printf(app, "Downloaded %lu bytes\n", file_size);
    } else {
        storage_simply_remove(app->storage, dest);
        terminal_println(app, "Download failed");
    }
}

// --- Package Manager ---

static void pkg_trim(char* str) {
    size_t len = strlen(str);
    while(len > 0 && (str[len - 1] == '\n' || str[len - 1] == '\r' || str[len - 1] == ' ')) {
        str[--len] = '\0';
    }
}

static void pkg_load_repo_url(TerminalApp* app) {
    const char* default_url =
        "https://raw.githubusercontent.com/DuckyScript/flipped/dev/app_install";

    File* file = storage_file_alloc(app->storage);
    const char* config_path = PKG_INSTALL_PATH "/repo_url.txt";

    if(storage_file_open(file, config_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char url_buf[WIFI_MAX_URL_LEN];
        size_t bytes = storage_file_read(file, url_buf, sizeof(url_buf) - 1);
        if(bytes > 0) {
            url_buf[bytes] = '\0';
            pkg_trim(url_buf);
            if(strlen(url_buf) > 0) {
                strlcpy(app->pkg.repo_url, url_buf, sizeof(app->pkg.repo_url));
                storage_file_close(file);
                storage_file_free(file);
                return;
            }
        }
        storage_file_close(file);
    }
    storage_file_free(file);
    strlcpy(app->pkg.repo_url, default_url, sizeof(app->pkg.repo_url));
}

static bool pkg_parse_manifest(TerminalApp* app) {
    app->pkg.count = 0;

    File* file = storage_file_alloc(app->storage);
    if(!storage_file_open(file, PKG_MANIFEST, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(file);
        return false;
    }

    char line[PKG_MANIFEST_LINE];
    size_t line_pos = 0;
    char ch;

    while(app->pkg.count < PKG_MAX_APPS) {
        size_t bytes_read = storage_file_read(file, &ch, 1);
        if(bytes_read == 0) {
            if(line_pos > 0) {
                line[line_pos] = '\0';
            } else {
                break;
            }
        } else if(ch == '\n') {
            line[line_pos] = '\0';
            line_pos = 0;
        } else {
            if(line_pos < PKG_MANIFEST_LINE - 1) {
                line[line_pos++] = ch;
            }
            if(bytes_read > 0) continue;
        }

        if(line[0] == '#' || line[0] == '\0') {
            if(bytes_read == 0) break;
            continue;
        }

        // Parse: name|filename|category|description
        PkgEntry* entry = &app->pkg.apps[app->pkg.count];
        char* ptr = line;
        char* field_start = ptr;
        int field = 0;

        while(*ptr && field < 4) {
            if(*ptr == '|' || *(ptr + 1) == '\0') {
                if(*(ptr + 1) == '\0' && *ptr != '|') ptr++;
                char saved = *ptr;
                *ptr = '\0';

                switch(field) {
                case 0:
                    strlcpy(entry->name, field_start, PKG_MAX_NAME);
                    pkg_trim(entry->name);
                    break;
                case 1:
                    strlcpy(entry->filename, field_start, PKG_MAX_FILE);
                    pkg_trim(entry->filename);
                    break;
                case 2:
                    strlcpy(entry->category, field_start, PKG_MAX_CAT);
                    pkg_trim(entry->category);
                    break;
                case 3:
                    strlcpy(entry->description, field_start, PKG_MAX_DESC);
                    pkg_trim(entry->description);
                    break;
                }

                *ptr = saved;
                field_start = ptr + 1;
                field++;
            }
            ptr++;
        }

        if(field >= 3 && strlen(entry->name) > 0 && strlen(entry->filename) > 0) {
            app->pkg.count++;
        }

        if(bytes_read == 0) break;
    }

    storage_file_close(file);
    storage_file_free(file);
    app->pkg.loaded = true;
    return true;
}

static bool pkg_is_installed(TerminalApp* app, PkgEntry* entry) {
    char path[DIR_NAME_MAX];
    snprintf(path, sizeof(path), PKG_APPS_DIR "/%s/%s", entry->category, entry->filename);
    FileInfo finfo;
    return storage_common_stat(app->storage, path, &finfo) == FSE_OK;
}

static bool pkg_has_local(TerminalApp* app, PkgEntry* entry) {
    char path[DIR_NAME_MAX];
    snprintf(path, sizeof(path), PKG_INSTALL_PATH "/%s", entry->filename);
    FileInfo finfo;
    return storage_common_stat(app->storage, path, &finfo) == FSE_OK;
}

// Download a file via WiFi (reuses wifi protocol)
static bool pkg_wifi_download(TerminalApp* app, const char* url, const char* dest) {
    if(!app->wifi.connected) return false;

    wifi_flush(&app->wifi);
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "DOWNLOAD %s\r\n", url);
    wifi_send(&app->wifi, cmd);

    char resp[128];
    size_t len = wifi_read_line(&app->wifi, resp, sizeof(resp), WIFI_CMD_TIMEOUT_MS);
    if(len == 0 || strncmp(resp, "SIZE:", 5) != 0) {
        if(strncmp(resp, "ERROR:", 6) == 0) {
            FURI_LOG_E(TAG, "Download error: %s", resp + 6);
        }
        return false;
    }

    uint32_t file_size = strtoul(resp + 5, NULL, 10);
    if(file_size == 0 || file_size > 1024 * 1024) return false;

    File* file = storage_file_alloc(app->storage);
    if(!storage_file_open(file, dest, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(file);
        return false;
    }

    uint32_t remaining = file_size;
    bool ok = true;

    while(remaining > 0) {
        uint8_t chunk[512];
        size_t to_read = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
        uint32_t start = furi_get_tick();
        size_t total_chunk = 0;

        while(total_chunk < to_read) {
            uint32_t elapsed =
                (furi_get_tick() - start) * 1000 / furi_kernel_get_tick_frequency();
            if(elapsed >= 30000) {
                ok = false;
                break;
            }
            size_t got = furi_stream_buffer_receive(
                app->wifi.rx_stream, chunk + total_chunk, to_read - total_chunk, 50);
            total_chunk += got;
        }
        if(!ok) break;

        if(storage_file_write(file, chunk, total_chunk) != total_chunk) {
            ok = false;
            break;
        }
        remaining -= total_chunk;
    }

    storage_file_close(file);
    storage_file_free(file);

    if(!ok) {
        storage_simply_remove(app->storage, dest);
        return false;
    }

    // Consume trailing DONE
    wifi_read_line(&app->wifi, resp, sizeof(resp), 2000);
    return true;
}

static PkgEntry* pkg_find(TerminalApp* app, const char* name) {
    for(uint32_t i = 0; i < app->pkg.count; i++) {
        if(strcasecmp(app->pkg.apps[i].name, name) == 0) {
            return &app->pkg.apps[i];
        }
        // Also match by filename without extension
        char fname[PKG_MAX_FILE];
        strlcpy(fname, app->pkg.apps[i].filename, sizeof(fname));
        char* dot = strrchr(fname, '.');
        if(dot) *dot = '\0';
        if(strcasecmp(fname, name) == 0) {
            return &app->pkg.apps[i];
        }
    }
    return NULL;
}

static void cmd_pkg(TerminalApp* app, const char* args) {
    if(!args || strlen(args) == 0) {
        terminal_println(app, "Package Manager");
        terminal_println(app, "usage: pkg <command>");
        terminal_println(app, "");
        terminal_println(app, " pkg list     - list all packages");
        terminal_println(app, " pkg search <q> - search packages");
        terminal_println(app, " pkg info <n> - package details");
        terminal_println(app, " pkg install <n> - install pkg");
        terminal_println(app, " pkg remove <n>  - remove pkg");
        terminal_println(app, " pkg update   - refresh catalog");
        terminal_println(app, " pkg upgrade  - install all");
        terminal_println(app, " pkg repo [url] - show/set repo");
        return;
    }

    // Parse subcommand
    char subcmd[32];
    const char* subargs = NULL;
    const char* space = strchr(args, ' ');
    if(space) {
        size_t slen = space - args;
        if(slen >= sizeof(subcmd)) slen = sizeof(subcmd) - 1;
        memcpy(subcmd, args, slen);
        subcmd[slen] = '\0';
        subargs = space + 1;
        while(*subargs == ' ') subargs++;
        if(*subargs == '\0') subargs = NULL;
    } else {
        strlcpy(subcmd, args, sizeof(subcmd));
    }

    // Ensure manifest is loaded
    if(!app->pkg.loaded) {
        pkg_parse_manifest(app);
    }

    if(strcmp(subcmd, "list") == 0 || strcmp(subcmd, "ls") == 0) {
        if(app->pkg.count == 0) {
            terminal_println(app, "No packages in catalog");
            terminal_println(app, "Run 'pkg update' to refresh");
            return;
        }
        for(uint32_t i = 0; i < app->pkg.count; i++) {
            PkgEntry* e = &app->pkg.apps[i];
            bool installed = pkg_is_installed(app, e);
            terminal_printf(
                app,
                " %s %s [%s]\n",
                installed ? "[*]" : "[ ]",
                e->name,
                e->category);
        }
        terminal_printf(app, "%lu packages\n", app->pkg.count);

    } else if(strcmp(subcmd, "search") == 0 || strcmp(subcmd, "find") == 0) {
        if(!subargs) {
            terminal_println(app, "usage: pkg search <query>");
            return;
        }
        uint32_t found = 0;
        for(uint32_t i = 0; i < app->pkg.count; i++) {
            PkgEntry* e = &app->pkg.apps[i];
            // Case-insensitive search in name, category, description
            bool match = false;
            // Simple case-insensitive substring search
            FuriString* haystack = furi_string_alloc();
            FuriString* needle = furi_string_alloc_set_str(subargs);

            furi_string_set_str(haystack, e->name);
            if(furi_string_search_str(haystack, subargs, 0) != FURI_STRING_FAILURE) match = true;
            furi_string_set_str(haystack, e->category);
            if(furi_string_search_str(haystack, subargs, 0) != FURI_STRING_FAILURE) match = true;
            furi_string_set_str(haystack, e->description);
            if(furi_string_search_str(haystack, subargs, 0) != FURI_STRING_FAILURE) match = true;

            furi_string_free(haystack);
            furi_string_free(needle);

            if(match) {
                bool installed = pkg_is_installed(app, e);
                terminal_printf(
                    app,
                    " %s %s - %s\n",
                    installed ? "[*]" : "[ ]",
                    e->name,
                    e->description);
                found++;
            }
        }
        if(found == 0) {
            terminal_printf(app, "No packages matching '%s'\n", subargs);
        } else {
            terminal_printf(app, "%lu found\n", found);
        }

    } else if(strcmp(subcmd, "info") == 0) {
        if(!subargs) {
            terminal_println(app, "usage: pkg info <name>");
            return;
        }
        PkgEntry* e = pkg_find(app, subargs);
        if(!e) {
            terminal_printf(app, "Package '%s' not found\n", subargs);
            return;
        }
        bool installed = pkg_is_installed(app, e);
        bool local = pkg_has_local(app, e);
        terminal_printf(app, "Name: %s\n", e->name);
        terminal_printf(app, "File: %s\n", e->filename);
        terminal_printf(app, "Category: %s\n", e->category);
        terminal_printf(app, "Desc: %s\n", e->description);
        terminal_printf(app, "Installed: %s\n", installed ? "yes" : "no");
        terminal_printf(app, "Cached: %s\n", local ? "yes" : "no");

    } else if(strcmp(subcmd, "install") == 0 || strcmp(subcmd, "i") == 0) {
        if(!subargs) {
            terminal_println(app, "usage: pkg install <name>");
            return;
        }
        PkgEntry* e = pkg_find(app, subargs);
        if(!e) {
            terminal_printf(app, "Package '%s' not found\n", subargs);
            terminal_println(app, "Run 'pkg list' to see available");
            return;
        }

        bool has_local = pkg_has_local(app, e);

        // If no local .fap, try WiFi download
        if(!has_local) {
            if(!app->wifi.connected) {
                terminal_println(app, "No local .fap and no WiFi");
                terminal_println(app, "Copy .fap to SD or use WiFi");
                return;
            }

            terminal_printf(app, "Downloading %s...\n", e->filename);
            char url[WIFI_MAX_URL_LEN];
            snprintf(url, sizeof(url), "%s/%s", app->pkg.repo_url, e->filename);

            char local_path[DIR_NAME_MAX];
            snprintf(local_path, sizeof(local_path), PKG_INSTALL_PATH "/%s", e->filename);
            storage_simply_mkdir(app->storage, PKG_INSTALL_PATH);

            if(!pkg_wifi_download(app, url, local_path)) {
                terminal_println(app, "Download failed");
                return;
            }
            terminal_println(app, "Downloaded");
        }

        // Copy from app_install to apps/<category>/
        char src[DIR_NAME_MAX];
        snprintf(src, sizeof(src), PKG_INSTALL_PATH "/%s", e->filename);

        char dest_dir[DIR_NAME_MAX];
        snprintf(dest_dir, sizeof(dest_dir), PKG_APPS_DIR "/%s", e->category);
        storage_simply_mkdir(app->storage, PKG_APPS_DIR);
        storage_simply_mkdir(app->storage, dest_dir);

        char dest[DIR_NAME_MAX];
        snprintf(dest, sizeof(dest), "%s/%s", dest_dir, e->filename);
        storage_simply_remove(app->storage, dest);

        FS_Error err = storage_common_copy(app->storage, src, dest);
        if(err == FSE_OK) {
            terminal_printf(app, "Installed %s\n", e->name);
        } else {
            terminal_printf(app, "Install failed (err %d)\n", err);
        }

    } else if(strcmp(subcmd, "remove") == 0 || strcmp(subcmd, "rm") == 0 ||
              strcmp(subcmd, "uninstall") == 0) {
        if(!subargs) {
            terminal_println(app, "usage: pkg remove <name>");
            return;
        }
        PkgEntry* e = pkg_find(app, subargs);
        if(!e) {
            terminal_printf(app, "Package '%s' not found\n", subargs);
            return;
        }
        if(!pkg_is_installed(app, e)) {
            terminal_printf(app, "%s is not installed\n", e->name);
            return;
        }

        char path[DIR_NAME_MAX];
        snprintf(path, sizeof(path), PKG_APPS_DIR "/%s/%s", e->category, e->filename);
        if(storage_simply_remove(app->storage, path)) {
            terminal_printf(app, "Removed %s\n", e->name);
        } else {
            terminal_println(app, "Remove failed");
        }

    } else if(strcmp(subcmd, "update") == 0 || strcmp(subcmd, "refresh") == 0) {
        if(!app->wifi.connected) {
            terminal_println(app, "WiFi not connected");
            terminal_println(app, "Use 'connect' first");
            // Try loading local manifest instead
            if(pkg_parse_manifest(app)) {
                terminal_printf(app, "Loaded %lu local packages\n", app->pkg.count);
            }
            return;
        }

        terminal_println(app, "Updating catalog...");
        char url[WIFI_MAX_URL_LEN];
        snprintf(url, sizeof(url), "%s/manifest.txt", app->pkg.repo_url);
        storage_simply_mkdir(app->storage, PKG_INSTALL_PATH);

        if(pkg_wifi_download(app, url, PKG_MANIFEST)) {
            app->pkg.loaded = false;
            pkg_parse_manifest(app);
            terminal_printf(app, "Catalog updated: %lu packages\n", app->pkg.count);
        } else {
            terminal_println(app, "Update failed");
        }

    } else if(strcmp(subcmd, "upgrade") == 0) {
        if(app->pkg.count == 0) {
            terminal_println(app, "No packages. Run 'pkg update'");
            return;
        }

        uint32_t installed = 0;
        uint32_t failed = 0;

        for(uint32_t i = 0; i < app->pkg.count; i++) {
            PkgEntry* e = &app->pkg.apps[i];
            bool has_local = pkg_has_local(app, e);

            // Download if missing and WiFi available
            if(!has_local && app->wifi.connected) {
                terminal_printf(app, "Downloading %s...\n", e->name);
                char url[WIFI_MAX_URL_LEN];
                snprintf(url, sizeof(url), "%s/%s", app->pkg.repo_url, e->filename);
                char local_path[DIR_NAME_MAX];
                snprintf(local_path, sizeof(local_path), PKG_INSTALL_PATH "/%s", e->filename);
                storage_simply_mkdir(app->storage, PKG_INSTALL_PATH);

                if(!pkg_wifi_download(app, url, local_path)) {
                    terminal_printf(app, "  Failed to download %s\n", e->name);
                    failed++;
                    continue;
                }
                has_local = true;
            }

            if(!has_local) {
                terminal_printf(app, "  Skipping %s (no .fap)\n", e->name);
                failed++;
                continue;
            }

            // Install
            char src[DIR_NAME_MAX];
            snprintf(src, sizeof(src), PKG_INSTALL_PATH "/%s", e->filename);
            char dest_dir[DIR_NAME_MAX];
            snprintf(dest_dir, sizeof(dest_dir), PKG_APPS_DIR "/%s", e->category);
            storage_simply_mkdir(app->storage, PKG_APPS_DIR);
            storage_simply_mkdir(app->storage, dest_dir);

            char dest[DIR_NAME_MAX];
            snprintf(dest, sizeof(dest), "%s/%s", dest_dir, e->filename);
            storage_simply_remove(app->storage, dest);

            if(storage_common_copy(app->storage, src, dest) == FSE_OK) {
                terminal_printf(app, "  Installed %s\n", e->name);
                installed++;
            } else {
                terminal_printf(app, "  Failed %s\n", e->name);
                failed++;
            }
        }

        terminal_printf(app, "Done: %lu installed, %lu failed\n", installed, failed);

    } else if(strcmp(subcmd, "repo") == 0) {
        if(subargs) {
            strlcpy(app->pkg.repo_url, subargs, sizeof(app->pkg.repo_url));
            // Save to file
            File* file = storage_file_alloc(app->storage);
            const char* config_path = PKG_INSTALL_PATH "/repo_url.txt";
            storage_simply_mkdir(app->storage, PKG_INSTALL_PATH);
            if(storage_file_open(file, config_path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
                storage_file_write(file, subargs, strlen(subargs));
                storage_file_close(file);
                terminal_println(app, "Repo URL saved");
            } else {
                terminal_println(app, "Failed to save URL");
            }
            storage_file_free(file);
        }
        terminal_printf(app, "Repo: %s\n", app->pkg.repo_url);

    } else {
        terminal_printf(app, "pkg: unknown command '%s'\n", subcmd);
        terminal_println(app, "Run 'pkg' for help");
    }
}

// --- Command Dispatcher ---

static void terminal_execute(TerminalApp* app, const char* input) {
    terminal_printf(app, "> %s\n", input);

    char cmd_copy[CMD_BUF_SIZE];
    strlcpy(cmd_copy, input, sizeof(cmd_copy));

    char* cmd = cmd_copy;
    while(*cmd == ' ') cmd++;
    size_t clen = strlen(cmd);
    while(clen > 0 && cmd[clen - 1] == ' ') cmd[--clen] = '\0';
    if(clen == 0) return;

    char* args = strchr(cmd, ' ');
    if(args) {
        *args = '\0';
        args++;
        while(*args == ' ') args++;
        if(*args == '\0') args = NULL;
    }

    if(strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        cmd_help(app);
    } else if(strcmp(cmd, "ls") == 0 || strcmp(cmd, "dir") == 0) {
        cmd_ls(app, args);
    } else if(strcmp(cmd, "cat") == 0 || strcmp(cmd, "type") == 0) {
        cmd_cat(app, args);
    } else if(strcmp(cmd, "rm") == 0 || strcmp(cmd, "del") == 0) {
        cmd_rm(app, args);
    } else if(strcmp(cmd, "mkdir") == 0) {
        cmd_mkdir(app, args);
    } else if(strcmp(cmd, "mv") == 0 || strcmp(cmd, "ren") == 0) {
        cmd_mv(app, args);
    } else if(strcmp(cmd, "cp") == 0 || strcmp(cmd, "copy") == 0) {
        cmd_cp(app, args);
    } else if(strcmp(cmd, "stat") == 0) {
        cmd_stat(app, args);
    } else if(strcmp(cmd, "df") == 0) {
        cmd_df(app);
    } else if(strcmp(cmd, "pwd") == 0) {
        terminal_printf(app, "%s\n", app->cwd);
    } else if(strcmp(cmd, "cd") == 0) {
        cmd_cd(app, args);
    } else if(strcmp(cmd, "clear") == 0 || strcmp(cmd, "cls") == 0) {
        furi_string_reset(app->output);
    } else if(strcmp(cmd, "info") == 0) {
        cmd_info(app);
    } else if(strcmp(cmd, "ping") == 0) {
        cmd_ping(app);
    } else if(strcmp(cmd, "wifi") == 0) {
        cmd_wifi_status(app);
    } else if(strcmp(cmd, "ssid") == 0) {
        cmd_wifi_ssid(app, args);
    } else if(strcmp(cmd, "pass") == 0) {
        cmd_wifi_pass(app, args);
    } else if(strcmp(cmd, "connect") == 0) {
        cmd_wifi_connect(app);
    } else if(strcmp(cmd, "wget") == 0) {
        cmd_wget(app, args);
    } else if(strcmp(cmd, "pkg") == 0 || strcmp(cmd, "apt") == 0) {
        cmd_pkg(app, args);
    } else {
        terminal_printf(app, "%s: unknown command\n", cmd);
        terminal_println(app, "Type 'help' for commands");
    }
}

// --- UI Callbacks ---

static uint32_t terminal_exit_callback(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static uint32_t terminal_home_callback(void* context) {
    UNUSED(context);
    return TerminalViewHome;
}

static void terminal_input_done(void* context) {
    TerminalApp* app = context;
    terminal_execute(app, app->cmd_buf);
    app->cmd_buf[0] = '\0';
    terminal_update_textbox(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, TerminalViewOutput);
}

static void terminal_menu_callback(void* context, uint32_t index) {
    TerminalApp* app = context;

    if(index == TerminalMenuEnterCmd) {
        text_input_reset(app->text_input);
        text_input_set_header_text(app->text_input, "$ Command:");
        text_input_set_result_callback(
            app->text_input,
            terminal_input_done,
            app,
            app->cmd_buf,
            CMD_BUF_SIZE,
            true);
        view_dispatcher_switch_to_view(app->view_dispatcher, TerminalViewInput);
    } else if(index == TerminalMenuViewOutput) {
        terminal_update_textbox(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, TerminalViewOutput);
    } else if(index == TerminalMenuClear) {
        furi_string_reset(app->output);
        terminal_println(app, "Output cleared");
        notification_message(app->notifications, &sequence_success);
    }
}

// --- App Lifecycle ---

static TerminalApp* terminal_alloc(void) {
    TerminalApp* app = malloc(sizeof(TerminalApp));
    memset(app, 0, sizeof(TerminalApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->output = furi_string_alloc();
    strlcpy(app->cwd, "/ext", sizeof(app->cwd));

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_enable_queue(app->view_dispatcher);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    // Submenu (home)
    app->submenu = submenu_alloc();
    submenu_add_item(app->submenu, "Enter Command", TerminalMenuEnterCmd, terminal_menu_callback, app);
    submenu_add_item(app->submenu, "View Output", TerminalMenuViewOutput, terminal_menu_callback, app);
    submenu_add_item(app->submenu, "Clear Output", TerminalMenuClear, terminal_menu_callback, app);
    submenu_set_header(app->submenu, "Terminal");
    View* sub_view = submenu_get_view(app->submenu);
    view_set_previous_callback(sub_view, terminal_exit_callback);
    view_dispatcher_add_view(app->view_dispatcher, TerminalViewHome, sub_view);

    // TextBox (output)
    app->text_box = text_box_alloc();
    text_box_set_font(app->text_box, TextBoxFontText);
    text_box_set_focus(app->text_box, TextBoxFocusEnd);
    View* tb_view = text_box_get_view(app->text_box);
    view_set_previous_callback(tb_view, terminal_home_callback);
    view_dispatcher_add_view(app->view_dispatcher, TerminalViewOutput, tb_view);

    // TextInput (command entry)
    app->text_input = text_input_alloc();
    View* ti_view = text_input_get_view(app->text_input);
    view_set_previous_callback(ti_view, terminal_home_callback);
    view_dispatcher_add_view(app->view_dispatcher, TerminalViewInput, ti_view);

    return app;
}

static void terminal_free(TerminalApp* app) {
    wifi_deinit(&app->wifi);

    view_dispatcher_remove_view(app->view_dispatcher, TerminalViewHome);
    view_dispatcher_remove_view(app->view_dispatcher, TerminalViewOutput);
    view_dispatcher_remove_view(app->view_dispatcher, TerminalViewInput);

    submenu_free(app->submenu);
    text_box_free(app->text_box);
    text_input_free(app->text_input);
    furi_string_free(app->output);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_NOTIFICATION);

    free(app);
}

int32_t terminal_app(void* p) {
    UNUSED(p);

    TerminalApp* app = terminal_alloc();

    // Load package manager config
    pkg_load_repo_url(app);

    // Try WiFi
    if(wifi_init(&app->wifi)) {
        wifi_detect(&app->wifi);
    }

    // Welcome
    terminal_println(app, "Flipper Terminal v1.0");
    terminal_println(app, "Type 'help' for commands");
    if(app->wifi.detected) {
        if(app->wifi.connected) {
            terminal_println(app, "WiFi: connected");
        } else {
            terminal_println(app, "WiFi: module found");
        }
    }
    terminal_printf(app, "cwd: %s\n", app->cwd);

    view_dispatcher_switch_to_view(app->view_dispatcher, TerminalViewHome);
    view_dispatcher_run(app->view_dispatcher);

    terminal_free(app);
    return 0;
}
