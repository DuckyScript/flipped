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
