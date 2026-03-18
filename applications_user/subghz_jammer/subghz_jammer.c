#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/variable_item_list.h>
#include <notification/notification_messages.h>

/* Register configurations for the CC1101 */
/* These are standard presets from the Flipper firmware, modified for jamming */
static const uint8_t jammer_cw_regs[] = {
    0x02, 0x0D, /* IOCFG0: GD0 as TX data (transparent) */
    0x08, 0x00, /* PKTCTRL1: No address check */
    0x07, 0x00, /* PKTCTRL0: Fixed packet length mode */
    0x12, 0x30, /* MDMCFG4: 325kHz Bandwidth */
    0x11, 0xF8, /* MDMCFG3: Symbol rate 250kBaud */
    0x10, 0x00, /* MDMCFG2: 2-FSK, No Manchester, No Sync */
    0x15, 0x40, /* DEVIATION: 47kHz */
    0x00, 0x00, /* End of registers */
};

/* Noise mode uses a pseudo-random sequence if possible, or high bitrate OOK */
static const uint8_t jammer_noise_regs[] = {
    0x02, 0x0D, /* IOCFG0: GD0 as TX data (transparent) */
    0x08, 0x00, /* PKTCTRL1 */
    0x07, 0x00, /* PKTCTRL0 */
    0x12, 0x30, /* MDMCFG4: 325kHz Bandwidth */
    0x11, 0xF8, /* MDMCFG3 */
    0x10, 0x30, /* MDMCFG2: ASK/OOK modulation */
    0x00, 0x00, /* End of registers */
};

typedef enum {
    JamModeCW,
    JamModeNoise,
} JamMode;

static const char* const jam_mode_names[] = {
    "Carrier (CW)",
    "Noise (OOK)",
};

static const uint32_t jam_frequencies[] = {
    315000000,
    433920000,
    868350000,
    915000000,
};

static const char* const jam_freq_names[] = {
    "315.00 MHz",
    "433.92 MHz",
    "868.35 MHz",
    "915.00 MHz",
};

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    VariableItemList* variable_item_list;
    NotificationApp* notifications;

    uint32_t frequency_index;
    JamMode mode;
    bool is_jamming;
} SubGhzJammerApp;

static void subghz_jammer_start(SubGhzJammerApp* app) {
    if(app->is_jamming) return;

    /* Initialize radio */
    furi_hal_subghz_reset();
    furi_hal_subghz_load_custom_preset(app->mode == JamModeCW ? jammer_cw_regs : jammer_noise_regs);
    furi_hal_subghz_set_frequency(jam_frequencies[app->frequency_index]);

    /* Start transmission */
    if(furi_hal_subghz_tx()) {
        app->is_jamming = true;
        notification_message(app->notifications, &sequence_set_red_255);
    } else {
        notification_message(app->notifications, &sequence_error);
    }
}

static void subghz_jammer_stop(SubGhzJammerApp* app) {
    if(!app->is_jamming) return;

    furi_hal_subghz_idle();
    furi_hal_subghz_sleep();
    app->is_jamming = false;
    notification_message(app->notifications, &sequence_reset_red);
}

static void subghz_jammer_freq_change(VariableItem* item) {
    SubGhzJammerApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, jam_freq_names[index]);
    app->frequency_index = index;

    if(app->is_jamming) {
        subghz_jammer_stop(app);
        subghz_jammer_start(app);
    }
}

static void subghz_jammer_mode_change(VariableItem* item) {
    SubGhzJammerApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, jam_mode_names[index]);
    app->mode = (JamMode)index;

    if(app->is_jamming) {
        subghz_jammer_stop(app);
        subghz_jammer_start(app);
    }
}

static void subghz_jammer_status_change(VariableItem* item) {
    SubGhzJammerApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, index ? "ON" : "OFF");

    if(index) {
        subghz_jammer_start(app);
    } else {
        subghz_jammer_stop(app);
    }
}

static SubGhzJammerApp* subghz_jammer_app_alloc() {
    SubGhzJammerApp* app = malloc(sizeof(SubGhzJammerApp));
    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_enable_queue(app->view_dispatcher);

    app->variable_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, 0, variable_item_list_get_view(app->variable_item_list));
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->frequency_index = 1; /* 433.92 MHz */
    app->mode = JamModeNoise;
    app->is_jamming = false;

    VariableItem* item;
    item = variable_item_list_add(
        app->variable_item_list, "Frequency", 4, subghz_jammer_freq_change, app);
    variable_item_set_current_value_index(item, app->frequency_index);
    variable_item_set_current_value_text(item, jam_freq_names[app->frequency_index]);

    item = variable_item_list_add(app->variable_item_list, "Mode", 2, subghz_jammer_mode_change, app);
    variable_item_set_current_value_index(item, (uint8_t)app->mode);
    variable_item_set_current_value_text(item, jam_mode_names[app->mode]);

    item = variable_item_list_add(
        app->variable_item_list, "Jamming", 2, subghz_jammer_status_change, app);
    variable_item_set_current_value_index(item, 0);
    variable_item_set_current_value_text(item, "OFF");

    return app;
}

static void subghz_jammer_app_free(SubGhzJammerApp* app) {
    subghz_jammer_stop(app);

    view_dispatcher_remove_view(app->view_dispatcher, 0);
    variable_item_list_free(app->variable_item_list);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    free(app);
}

int32_t subghz_jammer_app(void* p) {
    UNUSED(p);
    SubGhzJammerApp* app = subghz_jammer_app_alloc();
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);
    view_dispatcher_run(app->view_dispatcher);
    subghz_jammer_app_free(app);
    return 0;
}
