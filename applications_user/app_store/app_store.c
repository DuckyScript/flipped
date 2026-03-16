#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/dialog_ex.h>
#include <gui/modules/popup.h>
#include <gui/modules/loading.h>
#include <input/input.h>
#include <storage/storage.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>

#define TAG "AppStore"

#define APP_INSTALL_PATH EXT_PATH("app_install")
#define APP_INSTALL_MANIFEST APP_INSTALL_PATH "/manifest.txt"
#define APPS_INSTALL_DIR EXT_PATH("apps")
#define MAX_APPS 32
#define MAX_NAME_LEN 64
#define MAX_FILE_LEN 128
#define MAX_CAT_LEN 32
#define MAX_DESC_LEN 128
#define MANIFEST_LINE_MAX 384

// WiFi UART protocol settings
#define WIFI_UART_BAUD 115200
#define WIFI_UART_TIMEOUT_MS 5000
#define WIFI_PING_TIMEOUT_MS 1000
#define WIFI_RX_BUF_SIZE 1024
#define WIFI_MAX_URL_LEN 512

// Menu item IDs (above MAX_APPS to avoid collision)
#define MENU_ID_WIFI_REFRESH 0xFE
#define MENU_ID_WIFI_STATUS 0xFF

typedef enum {
    AppStoreViewMenu,
    AppStoreViewInfo,
    AppStoreViewResult,
    AppStoreViewLoading,
} AppStoreView;

typedef struct {
    char name[MAX_NAME_LEN];
    char filename[MAX_FILE_LEN];
    char category[MAX_CAT_LEN];
    char description[MAX_DESC_LEN];
} AppEntry;

typedef struct {
    FuriHalSerialHandle* serial;
    FuriStreamBuffer* rx_stream;
    bool connected;
    bool module_detected;
} WifiModule;

typedef struct {
    Gui* gui;
    Storage* storage;
    NotificationApp* notifications;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    DialogEx* dialog;
    Popup* popup;
    Loading* loading;

    WifiModule wifi;

    AppEntry apps[MAX_APPS];
    uint32_t app_count;
    uint32_t selected_index;
    char repo_url[WIFI_MAX_URL_LEN];
} AppStoreApp;

// --- String Utilities ---

static void app_store_trim(char* str) {
    size_t len = strlen(str);
    while(len > 0 && (str[len - 1] == '\n' || str[len - 1] == '\r' || str[len - 1] == ' ')) {
        str[--len] = '\0';
    }
}

// --- WiFi Module Communication ---

static void wifi_rx_callback(
    FuriHalSerialHandle* handle,
    FuriHalSerialRxEvent event,
    void* context) {
    UNUSED(handle);
    WifiModule* wifi = context;

    if(event == FuriHalSerialRxEventData) {
        uint8_t data = furi_hal_serial_async_rx(handle);
        furi_stream_buffer_send(wifi->rx_stream, &data, 1, 0);
    }
}

static void wifi_flush_rx(WifiModule* wifi) {
    uint8_t tmp;
    while(furi_stream_buffer_receive(wifi->rx_stream, &tmp, 1, 0) > 0) {
    }
}

static bool wifi_init(WifiModule* wifi) {
    wifi->rx_stream = furi_stream_buffer_alloc(WIFI_RX_BUF_SIZE, 1);
    wifi->connected = false;
    wifi->module_detected = false;

    // Try USART (external GPIO header pins)
    wifi->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(!wifi->serial) {
        // Try LPUART as fallback
        wifi->serial = furi_hal_serial_control_acquire(FuriHalSerialIdLpuart);
    }

    if(!wifi->serial) {
        FURI_LOG_W(TAG, "No serial port available");
        furi_stream_buffer_free(wifi->rx_stream);
        wifi->rx_stream = NULL;
        return false;
    }

    furi_hal_serial_init(wifi->serial, WIFI_UART_BAUD);
    furi_hal_serial_async_rx_start(wifi->serial, wifi_rx_callback, wifi, false);

    return true;
}

static void wifi_deinit(WifiModule* wifi) {
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
    wifi->connected = false;
    wifi->module_detected = false;
}

static void wifi_send(WifiModule* wifi, const char* cmd) {
    furi_hal_serial_tx(
        wifi->serial, (const uint8_t*)cmd, strlen(cmd));
    furi_hal_serial_tx_wait_complete(wifi->serial);
}

static size_t wifi_read_line(WifiModule* wifi, char* buf, size_t buf_size, uint32_t timeout_ms) {
    uint32_t start = furi_get_tick();
    size_t pos = 0;

    while(pos < buf_size - 1) {
        uint32_t elapsed =
            (furi_get_tick() - start) * 1000 / furi_kernel_get_tick_frequency();
        if(elapsed >= timeout_ms) break;

        uint8_t ch;
        size_t received = furi_stream_buffer_receive(
            wifi->rx_stream, &ch, 1, 10);
        if(received == 0) continue;

        if(ch == '\n') {
            buf[pos] = '\0';
            app_store_trim(buf);
            return pos;
        }
        buf[pos++] = (char)ch;
    }

    buf[pos] = '\0';
    app_store_trim(buf);
    return pos;
}

static size_t wifi_read_bytes(
    WifiModule* wifi,
    uint8_t* buf,
    size_t count,
    uint32_t timeout_ms) {
    uint32_t start = furi_get_tick();
    size_t total = 0;

    while(total < count) {
        uint32_t elapsed =
            (furi_get_tick() - start) * 1000 / furi_kernel_get_tick_frequency();
        if(elapsed >= timeout_ms) break;

        size_t remaining = count - total;
        size_t chunk = remaining > 256 ? 256 : remaining;
        size_t received = furi_stream_buffer_receive(
            wifi->rx_stream, buf + total, chunk, 50);
        total += received;
    }

    return total;
}

static bool wifi_detect(WifiModule* wifi) {
    if(!wifi->serial) return false;

    wifi_flush_rx(wifi);

    // Send ping command
    wifi_send(wifi, "AT\r\n");

    char response[64];
    size_t len = wifi_read_line(wifi, response, sizeof(response), WIFI_PING_TIMEOUT_MS);

    if(len > 0 && strstr(response, "OK") != NULL) {
        wifi->module_detected = true;
        FURI_LOG_I(TAG, "WiFi module detected");

        // Check if WiFi is connected
        wifi_flush_rx(wifi);
        wifi_send(wifi, "WIFI_STATUS\r\n");
        len = wifi_read_line(wifi, response, sizeof(response), WIFI_PING_TIMEOUT_MS);
        if(len > 0 && strstr(response, "CONNECTED") != NULL) {
            wifi->connected = true;
            FURI_LOG_I(TAG, "WiFi connected");
        } else {
            wifi->connected = false;
            FURI_LOG_I(TAG, "WiFi not connected to network");
        }
        return true;
    }

    wifi->module_detected = false;
    FURI_LOG_I(TAG, "No WiFi module detected");
    return false;
}

// Download a file from URL to dest_path via WiFi module
// Protocol: DOWNLOAD <url>\r\n
// Response: SIZE:<bytes>\r\n followed by raw binary data, then \r\nDONE\r\n
// Or: ERROR:<message>\r\n
static bool wifi_download_file(
    WifiModule* wifi,
    Storage* storage,
    const char* url,
    const char* dest_path) {
    if(!wifi->serial || !wifi->connected) return false;

    wifi_flush_rx(wifi);

    // Send download command
    char cmd[WIFI_MAX_URL_LEN + 32];
    snprintf(cmd, sizeof(cmd), "DOWNLOAD %s\r\n", url);
    wifi_send(wifi, cmd);

    // Read response header
    char response[128];
    size_t len = wifi_read_line(wifi, response, sizeof(response), WIFI_UART_TIMEOUT_MS);

    if(len == 0) {
        FURI_LOG_E(TAG, "No response from WiFi module");
        return false;
    }

    if(strncmp(response, "ERROR:", 6) == 0) {
        FURI_LOG_E(TAG, "Download error: %s", response + 6);
        return false;
    }

    if(strncmp(response, "SIZE:", 5) != 0) {
        FURI_LOG_E(TAG, "Unexpected response: %s", response);
        return false;
    }

    uint32_t file_size = strtoul(response + 5, NULL, 10);
    if(file_size == 0 || file_size > 1024 * 1024) {
        FURI_LOG_E(TAG, "Invalid file size: %lu", file_size);
        return false;
    }

    FURI_LOG_I(TAG, "Downloading %lu bytes to %s", file_size, dest_path);

    // Open destination file
    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, dest_path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FURI_LOG_E(TAG, "Failed to create file: %s", dest_path);
        storage_file_free(file);
        return false;
    }

    // Read file data in chunks
    uint8_t chunk[512];
    uint32_t remaining = file_size;
    bool success = true;
    // Allow 30 seconds for large files
    uint32_t dl_timeout = 30000;

    while(remaining > 0) {
        size_t to_read = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
        size_t received = wifi_read_bytes(wifi, chunk, to_read, dl_timeout);

        if(received == 0) {
            FURI_LOG_E(TAG, "Download stalled, %lu bytes remaining", remaining);
            success = false;
            break;
        }

        size_t written = storage_file_write(file, chunk, received);
        if(written != received) {
            FURI_LOG_E(TAG, "Write error");
            success = false;
            break;
        }

        remaining -= received;
    }

    storage_file_close(file);
    storage_file_free(file);

    if(!success) {
        storage_simply_remove(storage, dest_path);
        return false;
    }

    // Read trailing DONE marker
    wifi_read_line(wifi, response, sizeof(response), 2000);

    FURI_LOG_I(TAG, "Download complete: %s", dest_path);
    return true;
}

// Download manifest from repo and parse it
static bool wifi_download_manifest(AppStoreApp* app) {
    if(!app->wifi.connected) return false;

    char url[WIFI_MAX_URL_LEN];
    snprintf(url, sizeof(url), "%s/manifest.txt", app->repo_url);

    // Download manifest to SD card
    if(!wifi_download_file(&app->wifi, app->storage, url, APP_INSTALL_MANIFEST)) {
        return false;
    }

    return true;
}

// Download a .fap file from repo
static bool wifi_download_app(AppStoreApp* app, uint32_t index) {
    if(index >= app->app_count) return false;
    if(!app->wifi.connected) return false;

    AppEntry* entry = &app->apps[index];

    char url[WIFI_MAX_URL_LEN];
    snprintf(url, sizeof(url), "%s/%s", app->repo_url, entry->filename);

    // Download to app_install directory first
    char local_path[256];
    snprintf(local_path, sizeof(local_path), APP_INSTALL_PATH "/%s", entry->filename);

    storage_simply_mkdir(app->storage, APP_INSTALL_PATH);

    return wifi_download_file(&app->wifi, app->storage, url, local_path);
}

// --- Manifest Parsing (from SD card) ---

static bool app_store_parse_manifest(AppStoreApp* app) {
    app->app_count = 0;

    File* file = storage_file_alloc(app->storage);
    if(!storage_file_open(file, APP_INSTALL_MANIFEST, FSAM_READ, FSOM_OPEN_EXISTING)) {
        FURI_LOG_E(TAG, "Failed to open manifest: %s", APP_INSTALL_MANIFEST);
        storage_file_free(file);
        return false;
    }

    char line[MANIFEST_LINE_MAX];
    size_t line_pos = 0;
    char ch;

    while(app->app_count < MAX_APPS) {
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
            if(line_pos < MANIFEST_LINE_MAX - 1) {
                line[line_pos++] = ch;
            }
            if(bytes_read > 0) continue;
        }

        if(line[0] == '#' || line[0] == '\0') {
            if(bytes_read == 0) break;
            continue;
        }

        // Parse: name|filename|category|description
        AppEntry* entry = &app->apps[app->app_count];
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
                    strlcpy(entry->name, field_start, MAX_NAME_LEN);
                    app_store_trim(entry->name);
                    break;
                case 1:
                    strlcpy(entry->filename, field_start, MAX_FILE_LEN);
                    app_store_trim(entry->filename);
                    break;
                case 2:
                    strlcpy(entry->category, field_start, MAX_CAT_LEN);
                    app_store_trim(entry->category);
                    break;
                case 3:
                    strlcpy(entry->description, field_start, MAX_DESC_LEN);
                    app_store_trim(entry->description);
                    break;
                }

                *ptr = saved;
                field_start = ptr + 1;
                field++;
            }
            ptr++;
        }

        if(field >= 3 && strlen(entry->name) > 0 && strlen(entry->filename) > 0) {
            FURI_LOG_I(TAG, "Found app: %s (%s)", entry->name, entry->filename);
            app->app_count++;
        }

        if(bytes_read == 0) break;
    }

    storage_file_close(file);
    storage_file_free(file);

    FURI_LOG_I(TAG, "Loaded %lu apps from manifest", app->app_count);
    return true;
}

// --- Local Install (copy from app_install to apps) ---

static bool app_store_install_local(AppStoreApp* app, uint32_t index) {
    if(index >= app->app_count) return false;

    AppEntry* entry = &app->apps[index];

    char src_path[256];
    snprintf(src_path, sizeof(src_path), APP_INSTALL_PATH "/%s", entry->filename);

    FileInfo file_info;
    if(storage_common_stat(app->storage, src_path, &file_info) != FSE_OK) {
        FURI_LOG_E(TAG, "Source not found: %s", src_path);
        return false;
    }

    char dest_dir[256];
    snprintf(dest_dir, sizeof(dest_dir), APPS_INSTALL_DIR "/%s", entry->category);

    storage_simply_mkdir(app->storage, APPS_INSTALL_DIR);
    storage_simply_mkdir(app->storage, dest_dir);

    char dest_path[256];
    snprintf(dest_path, sizeof(dest_path), "%s/%s", dest_dir, entry->filename);

    storage_simply_remove(app->storage, dest_path);

    FS_Error result = storage_common_copy(app->storage, src_path, dest_path);
    if(result != FSE_OK) {
        FURI_LOG_E(TAG, "Copy failed: %s -> %s (err %d)", src_path, dest_path, result);
        return false;
    }

    FURI_LOG_I(TAG, "Installed %s to %s", entry->name, dest_path);
    return true;
}

// WiFi install: download .fap from repo then copy to apps dir
static bool app_store_install_wifi(AppStoreApp* app, uint32_t index) {
    if(!wifi_download_app(app, index)) {
        return false;
    }
    return app_store_install_local(app, index);
}

static bool app_store_is_installed(AppStoreApp* app, uint32_t index) {
    if(index >= app->app_count) return false;

    AppEntry* entry = &app->apps[index];
    char dest_path[256];
    snprintf(
        dest_path,
        sizeof(dest_path),
        APPS_INSTALL_DIR "/%s/%s",
        entry->category,
        entry->filename);

    FileInfo file_info;
    return storage_common_stat(app->storage, dest_path, &file_info) == FSE_OK;
}

static bool app_store_has_local_fap(AppStoreApp* app, uint32_t index) {
    if(index >= app->app_count) return false;

    AppEntry* entry = &app->apps[index];
    char src_path[256];
    snprintf(src_path, sizeof(src_path), APP_INSTALL_PATH "/%s", entry->filename);

    FileInfo file_info;
    return storage_common_stat(app->storage, src_path, &file_info) == FSE_OK;
}

// --- UI Callbacks ---

static void app_store_menu_callback(void* context, uint32_t index);
static void app_store_refresh_menu(AppStoreApp* app);

static void app_store_show_result(AppStoreApp* app, const char* message) {
    popup_set_header(app->popup, message, 64, 20, AlignCenter, AlignCenter);
    popup_set_timeout(app->popup, 1500);
    popup_enable_timeout(app->popup);
    view_dispatcher_switch_to_view(app->view_dispatcher, AppStoreViewResult);
}

static void app_store_dialog_callback(DialogExResult result, void* context) {
    AppStoreApp* app = context;
    uint32_t index = app->selected_index;

    if(result == DialogExResultLeft) {
        view_dispatcher_switch_to_view(app->view_dispatcher, AppStoreViewMenu);
    } else if(result == DialogExResultRight) {
        // Install: prefer WiFi download if available, fallback to local
        bool success;
        if(app->wifi.connected && !app_store_has_local_fap(app, index)) {
            // No local .fap, download via WiFi
            view_dispatcher_switch_to_view(app->view_dispatcher, AppStoreViewLoading);
            success = app_store_install_wifi(app, index);
        } else if(app->wifi.connected) {
            // Has local .fap but WiFi available - just install local
            success = app_store_install_local(app, index);
        } else {
            success = app_store_install_local(app, index);
        }

        if(success) {
            notification_message(app->notifications, &sequence_success);
            app_store_show_result(app, "Installed!");
        } else {
            notification_message(app->notifications, &sequence_error);
            if(app->wifi.connected) {
                app_store_show_result(app, "Install failed!\nDownload error");
            } else {
                app_store_show_result(app, "Install failed!\n.fap not found");
            }
        }
        app_store_refresh_menu(app);
    } else if(result == DialogExResultCenter) {
        // Remove
        AppEntry* entry = &app->apps[index];
        char dest_path[256];
        snprintf(
            dest_path,
            sizeof(dest_path),
            APPS_INSTALL_DIR "/%s/%s",
            entry->category,
            entry->filename);
        if(storage_simply_remove(app->storage, dest_path)) {
            notification_message(app->notifications, &sequence_success);
            app_store_show_result(app, "Removed!");
        } else {
            notification_message(app->notifications, &sequence_error);
            app_store_show_result(app, "Remove failed!");
        }
        app_store_refresh_menu(app);
    }
}

static void app_store_menu_callback(void* context, uint32_t index) {
    AppStoreApp* app = context;

    // Handle WiFi-specific menu items
    if(index == MENU_ID_WIFI_REFRESH) {
        // Refresh catalog from WiFi
        view_dispatcher_switch_to_view(app->view_dispatcher, AppStoreViewLoading);

        bool success = wifi_download_manifest(app);
        if(success) {
            app_store_parse_manifest(app);
            app_store_refresh_menu(app);
            notification_message(app->notifications, &sequence_success);
            app_store_show_result(app, "Catalog updated!");
        } else {
            notification_message(app->notifications, &sequence_error);
            app_store_show_result(app, "Update failed!\nCheck WiFi");
            view_dispatcher_switch_to_view(app->view_dispatcher, AppStoreViewMenu);
        }
        return;
    }

    if(index == MENU_ID_WIFI_STATUS) {
        // Re-detect WiFi module
        view_dispatcher_switch_to_view(app->view_dispatcher, AppStoreViewLoading);
        wifi_detect(&app->wifi);
        app_store_refresh_menu(app);

        if(app->wifi.connected) {
            app_store_show_result(app, "WiFi connected!");
        } else if(app->wifi.module_detected) {
            app_store_show_result(app, "Module found\nNo WiFi network");
        } else {
            app_store_show_result(app, "No WiFi module");
        }
        return;
    }

    if(index >= app->app_count) return;

    app->selected_index = index;
    AppEntry* entry = &app->apps[index];
    bool installed = app_store_is_installed(app, index);
    bool has_local = app_store_has_local_fap(app, index);

    dialog_ex_set_header(app->dialog, entry->name, 64, 2, AlignCenter, AlignTop);

    // Show description with source info
    static char info_text[MAX_DESC_LEN + 32];
    if(app->wifi.connected && !has_local) {
        snprintf(info_text, sizeof(info_text), "%s\n[WiFi download]", entry->description);
    } else if(has_local) {
        snprintf(info_text, sizeof(info_text), "%s\n[SD card]", entry->description);
    } else {
        snprintf(info_text, sizeof(info_text), "%s\n[.fap missing]", entry->description);
    }
    dialog_ex_set_text(app->dialog, info_text, 64, 18, AlignCenter, AlignTop);

    if(installed) {
        dialog_ex_set_left_button_text(app->dialog, "Back");
        dialog_ex_set_right_button_text(app->dialog, "Reinstall");
        dialog_ex_set_center_button_text(app->dialog, "Remove");
    } else if(has_local || app->wifi.connected) {
        dialog_ex_set_left_button_text(app->dialog, "Back");
        dialog_ex_set_right_button_text(app->dialog, "Install");
        dialog_ex_set_center_button_text(app->dialog, NULL);
    } else {
        dialog_ex_set_left_button_text(app->dialog, "Back");
        dialog_ex_set_right_button_text(app->dialog, NULL);
        dialog_ex_set_center_button_text(app->dialog, NULL);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, AppStoreViewInfo);
}

static void app_store_refresh_menu(AppStoreApp* app) {
    submenu_reset(app->submenu);

    // WiFi status header item
    if(app->wifi.serial) {
        if(app->wifi.connected) {
            submenu_add_item(
                app->submenu,
                "[WiFi: Connected]",
                MENU_ID_WIFI_STATUS,
                app_store_menu_callback,
                app);
            submenu_add_item(
                app->submenu,
                ">> Refresh Catalog",
                MENU_ID_WIFI_REFRESH,
                app_store_menu_callback,
                app);
        } else if(app->wifi.module_detected) {
            submenu_add_item(
                app->submenu,
                "[WiFi: No Network]",
                MENU_ID_WIFI_STATUS,
                app_store_menu_callback,
                app);
        } else {
            submenu_add_item(
                app->submenu,
                "[WiFi: Not Detected]",
                MENU_ID_WIFI_STATUS,
                app_store_menu_callback,
                app);
        }
    }

    if(app->app_count == 0) {
        submenu_add_item(app->submenu, "[No apps in catalog]", 0, NULL, NULL);
    } else {
        for(uint32_t i = 0; i < app->app_count; i++) {
            static char labels[MAX_APPS][MAX_NAME_LEN + 16];
            bool installed = app_store_is_installed(app, i);
            snprintf(
                labels[i],
                sizeof(labels[i]),
                "%s%s",
                installed ? "[*] " : "    ",
                app->apps[i].name);
            submenu_add_item(app->submenu, labels[i], i, app_store_menu_callback, app);
        }
    }
}

static void app_store_popup_callback(void* context) {
    AppStoreApp* app = context;
    view_dispatcher_switch_to_view(app->view_dispatcher, AppStoreViewInfo);
}

static uint32_t app_store_exit_callback(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static uint32_t app_store_menu_back_callback(void* context) {
    UNUSED(context);
    return AppStoreViewMenu;
}

// --- Repo URL Config ---

static void app_store_load_config(AppStoreApp* app) {
    // Default repo URL - users can change this by editing the config file
    const char* default_url =
        "https://raw.githubusercontent.com/DuckyScript/flipped/dev/app_install";

    // Try to load custom URL from config file
    File* file = storage_file_alloc(app->storage);
    const char* config_path = APP_INSTALL_PATH "/repo_url.txt";

    if(storage_file_open(file, config_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char url_buf[WIFI_MAX_URL_LEN];
        size_t bytes = storage_file_read(file, url_buf, sizeof(url_buf) - 1);
        if(bytes > 0) {
            url_buf[bytes] = '\0';
            app_store_trim(url_buf);
            if(strlen(url_buf) > 0) {
                strlcpy(app->repo_url, url_buf, sizeof(app->repo_url));
                storage_file_close(file);
                storage_file_free(file);
                FURI_LOG_I(TAG, "Repo URL: %s", app->repo_url);
                return;
            }
        }
        storage_file_close(file);
    }
    storage_file_free(file);

    strlcpy(app->repo_url, default_url, sizeof(app->repo_url));
    FURI_LOG_I(TAG, "Using default repo URL: %s", app->repo_url);
}

// --- App Lifecycle ---

static AppStoreApp* app_store_alloc(void) {
    AppStoreApp* app = malloc(sizeof(AppStoreApp));
    memset(app, 0, sizeof(AppStoreApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_enable_queue(app->view_dispatcher);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    // Submenu (main app list)
    app->submenu = submenu_alloc();
    View* submenu_view = submenu_get_view(app->submenu);
    view_set_previous_callback(submenu_view, app_store_exit_callback);
    view_dispatcher_add_view(app->view_dispatcher, AppStoreViewMenu, submenu_view);

    // Dialog (app info / install confirm)
    app->dialog = dialog_ex_alloc();
    dialog_ex_set_result_callback(app->dialog, app_store_dialog_callback);
    dialog_ex_set_context(app->dialog, app);
    View* dialog_view = dialog_ex_get_view(app->dialog);
    view_set_previous_callback(dialog_view, app_store_menu_back_callback);
    view_dispatcher_add_view(app->view_dispatcher, AppStoreViewInfo, dialog_view);

    // Popup (result notification)
    app->popup = popup_alloc();
    popup_set_callback(app->popup, app_store_popup_callback);
    popup_set_context(app->popup, app);
    View* popup_view = popup_get_view(app->popup);
    view_set_previous_callback(popup_view, app_store_menu_back_callback);
    view_dispatcher_add_view(app->view_dispatcher, AppStoreViewResult, popup_view);

    // Loading spinner
    app->loading = loading_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, AppStoreViewLoading, loading_get_view(app->loading));

    return app;
}

static void app_store_free(AppStoreApp* app) {
    // Cleanup WiFi
    wifi_deinit(&app->wifi);

    view_dispatcher_remove_view(app->view_dispatcher, AppStoreViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, AppStoreViewInfo);
    view_dispatcher_remove_view(app->view_dispatcher, AppStoreViewResult);
    view_dispatcher_remove_view(app->view_dispatcher, AppStoreViewLoading);

    submenu_free(app->submenu);
    dialog_ex_free(app->dialog);
    popup_free(app->popup);
    loading_free(app->loading);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_NOTIFICATION);

    free(app);
}

int32_t app_store_app(void* p) {
    UNUSED(p);

    AppStoreApp* app = app_store_alloc();

    // Load repo URL config
    app_store_load_config(app);

    // Try to detect WiFi module
    if(wifi_init(&app->wifi)) {
        wifi_detect(&app->wifi);
    }

    // Parse local manifest and populate menu
    if(!app_store_parse_manifest(app)) {
        FURI_LOG_W(TAG, "No manifest found, showing empty store");
    }
    app_store_refresh_menu(app);

    if(app->app_count == 0) {
        submenu_set_header(app->submenu, "App Store (empty)");
    } else {
        submenu_set_header(app->submenu, "App Store");
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, AppStoreViewMenu);
    view_dispatcher_run(app->view_dispatcher);

    app_store_free(app);
    return 0;
}
