#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <furi_hal_bt.h>
#include <furi_hal_version.h>
#include "../../targets/f7/ble_glue/extra_beacon.h"

#define TAG "CustomBleBeacon"

typedef struct {
    FuriMessageQueue* input_queue;
    Gui* gui;
    ViewPort* view_port;
} CustomBleApp;

static void custom_ble_draw_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 10, 20, "Custom BLE Beacon");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 10, 40, "Status: Broadcasting...");
    canvas_draw_str(canvas, 10, 55, "Press Back to stop");
}

static void custom_ble_input_callback(InputEvent* input_event, void* context) {
    furi_assert(context);
    CustomBleApp* app = context;
    furi_message_queue_put(app->input_queue, input_event, FuriWaitForever);
}

static CustomBleApp* custom_ble_app_alloc() {
    CustomBleApp* app = malloc(sizeof(CustomBleApp));
    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, custom_ble_draw_callback, app);
    view_port_input_callback_set(app->view_port, custom_ble_input_callback, app);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    return app;
}

static void custom_ble_app_free(CustomBleApp* app) {
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_message_queue_free(app->input_queue);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t custom_ble_beacon_app(void* p) {
    UNUSED(p);
    CustomBleApp* app = custom_ble_app_alloc();

    // Configure Beacon
    GapExtraBeaconConfig config = {
        .min_adv_interval_ms = 100,
        .max_adv_interval_ms = 200,
        .adv_channel_map = GapAdvChannelMapAll,
        .adv_power_level = GapAdvPowerLevel_0dBm,
        .address_type = GapAddressTypePublic,
    };
    memcpy(config.address, furi_hal_version_get_ble_mac(), sizeof(config.address));
    config.address[0] ^= 0xFF; // Different MAC

    furi_hal_bt_extra_beacon_set_config(&config);

    // Set Data (Beacon Name "Flipper")
    uint8_t data[] = {
        0x02, 0x01, 0x06, // Flags
        0x08, 0x09, 'F', 'l', 'i', 'p', 'p', 'e', 'r' // Name
    };
    furi_hal_bt_extra_beacon_set_data(data, sizeof(data));

    // Start Beacon
    furi_hal_bt_extra_beacon_start();
    FURI_LOG_I(TAG, "Beacon started");

    InputEvent event;
    while(furi_message_queue_get(app->input_queue, &event, FuriWaitForever) == FuriStatusOk) {
        if(event.type == InputTypeShort && event.key == InputKeyBack) {
            break;
        }
    }

    // Stop Beacon
    furi_hal_bt_extra_beacon_stop();
    FURI_LOG_I(TAG, "Beacon stopped");

    custom_ble_app_free(app);
    return 0;
}
