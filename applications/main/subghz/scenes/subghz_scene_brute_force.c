#include "../subghz_i.h"
#include "../helpers/subghz_txrx_create_protocol_key.h"
#include <lib/subghz/protocols/protocol_items.h>
#include <dolphin/dolphin.h>

#define TAG "SubGhzBruteForce"

typedef struct {
    const char* protocol_name;
    const char* preset_name;
    uint32_t frequency;
    uint32_t total_bits;
    uint32_t total_keys;
    uint32_t te; // 0 = use default
} BruteForceProtocol;

static const BruteForceProtocol brute_force_protocols[] = {
    [0] = {"Princeton", "AM650", 433920000, 24, 0x100, 400},  // 8-bit device code, brute 256
    [1] = {"Nice FLO", "AM650", 433920000, 12, 0x1000, 0},    // 12-bit, 4096 keys
    [2] = {"CAME", "AM650", 433920000, 12, 0x1000, 0},        // 12-bit, 4096 keys
    [3] = {"Linear", "AM650", 300000000, 10, 0x400, 0},       // 10-bit, 1024 keys
    [4] = {"GateTX", "AM650", 433920000, 24, 0x100, 0},       // 8-bit brute portion
    [5] = {"Cham_Code", "AM650", 315000000, 9, 0x200, 0},     // 9-bit, 512 keys
    [6] = {"Cham_Code", "AM650", 390000000, 9, 0x200, 0},     // 9-bit, 512 keys
    [7] = {"LinearDelta3", "AM650", 310000000, 8, 0x100, 0},  // 8-bit, 256 keys
};

typedef struct {
    uint32_t current_key;
    uint32_t total_keys;
    uint8_t protocol_index;
    bool is_attacking;
} SubGhzBruteForceState;

static SubGhzBruteForceState bf_state = {0};

void subghz_scene_brute_force_submenu_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, index);
}

void subghz_scene_brute_force_on_enter(void* context) {
    SubGhz* subghz = context;

    bf_state.is_attacking = false;
    bf_state.current_key = 0;

    submenu_add_item(
        subghz->submenu,
        "Princeton 433 (8-bit)",
        SubmenuIndexBruteForcePrinceton_433,
        subghz_scene_brute_force_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Nice Flo 12bit 433",
        SubmenuIndexBruteForceNiceFlo_433,
        subghz_scene_brute_force_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "CAME 12bit 433",
        SubmenuIndexBruteForceCAME_433,
        subghz_scene_brute_force_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Linear 300",
        SubmenuIndexBruteForceLinear_300,
        subghz_scene_brute_force_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Gate TX 433",
        SubmenuIndexBruteForceGateTX_433,
        subghz_scene_brute_force_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Chamberlain 315",
        SubmenuIndexBruteForceChamberlin_315,
        subghz_scene_brute_force_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Chamberlain 390",
        SubmenuIndexBruteForceChamberlin_390,
        subghz_scene_brute_force_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Linear Delta3 310",
        SubmenuIndexBruteForceLinearDelta3_310,
        subghz_scene_brute_force_submenu_callback,
        subghz);

    submenu_set_selected_item(
        subghz->submenu,
        scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneBruteForce));

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
}

static uint8_t subghz_scene_brute_force_event_to_index(uint32_t event) {
    switch(event) {
    case SubmenuIndexBruteForcePrinceton_433:
        return 0;
    case SubmenuIndexBruteForceNiceFlo_433:
        return 1;
    case SubmenuIndexBruteForceCAME_433:
        return 2;
    case SubmenuIndexBruteForceLinear_300:
        return 3;
    case SubmenuIndexBruteForceGateTX_433:
        return 4;
    case SubmenuIndexBruteForceChamberlin_315:
        return 5;
    case SubmenuIndexBruteForceChamberlin_390:
        return 6;
    case SubmenuIndexBruteForceLinearDelta3_310:
        return 7;
    default:
        return 0xFF;
    }
}

static void subghz_scene_brute_force_start_attack(SubGhz* subghz, uint8_t protocol_index) {
    bf_state.protocol_index = protocol_index;
    bf_state.current_key = 0;
    bf_state.total_keys = brute_force_protocols[protocol_index].total_keys;
    bf_state.is_attacking = true;

    widget_reset(subghz->widget);
    widget_add_string_element(
        subghz->widget, 64, 2, AlignCenter, AlignTop, FontPrimary, "Brute Force");

    FuriString* status = furi_string_alloc();
    furi_string_printf(
        status,
        "%s %luMHz\nKey: 0/%lu\nPress Back to stop",
        brute_force_protocols[protocol_index].protocol_name,
        brute_force_protocols[protocol_index].frequency / 1000000,
        (unsigned long)bf_state.total_keys);
    widget_add_string_multiline_element(
        subghz->widget, 64, 18, AlignCenter, AlignTop, FontSecondary, furi_string_get_cstr(status));
    furi_string_free(status);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
}

static void subghz_scene_brute_force_update_progress(SubGhz* subghz) {
    widget_reset(subghz->widget);
    widget_add_string_element(
        subghz->widget, 64, 2, AlignCenter, AlignTop, FontPrimary, "Brute Force");

    uint8_t progress_percent = (bf_state.current_key * 100) / bf_state.total_keys;

    FuriString* status = furi_string_alloc();
    furi_string_printf(
        status,
        "%s %luMHz\nKey: %lu/%lu (%u%%)\nPress Back to stop",
        brute_force_protocols[bf_state.protocol_index].protocol_name,
        brute_force_protocols[bf_state.protocol_index].frequency / 1000000,
        (unsigned long)bf_state.current_key,
        (unsigned long)bf_state.total_keys,
        progress_percent);
    widget_add_string_multiline_element(
        subghz->widget, 64, 18, AlignCenter, AlignTop, FontSecondary, furi_string_get_cstr(status));
    furi_string_free(status);
}

static bool subghz_scene_brute_force_send_next_key(SubGhz* subghz) {
    if(bf_state.current_key >= bf_state.total_keys) {
        return false;
    }

    const BruteForceProtocol* proto = &brute_force_protocols[bf_state.protocol_index];
    SubGhzProtocolStatus status;

    uint64_t key = bf_state.current_key;

    // For GateTX, we need to reverse the key
    if(bf_state.protocol_index == 4) {
        key = (key & 0xFF) << 16 | 0xF0040; // format gate_tx style
    }

    if(proto->te != 0) {
        status = subghz_txrx_gen_data_protocol_and_te(
            subghz->txrx,
            proto->preset_name,
            proto->frequency,
            proto->protocol_name,
            key,
            proto->total_bits,
            proto->te);
    } else {
        status = subghz_txrx_gen_data_protocol(
            subghz->txrx,
            proto->preset_name,
            proto->frequency,
            proto->protocol_name,
            key,
            proto->total_bits);
    }

    if(status == SubGhzProtocolStatusOk) {
        if(subghz_tx_start(subghz, subghz_txrx_get_fff_data(subghz->txrx))) {
            // Wait for TX to complete
            furi_delay_ms(100);
            subghz_txrx_stop(subghz->txrx);
            bf_state.current_key++;
            return true;
        }
    }

    bf_state.current_key++;
    return true;
}

bool subghz_scene_brute_force_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;

    if(event.type == SceneManagerEventTypeBack) {
        if(bf_state.is_attacking) {
            bf_state.is_attacking = false;
            subghz_txrx_stop(subghz->txrx);
            subghz->state_notifications = SubGhzNotificationStateIDLE;
            scene_manager_search_and_switch_to_previous_scene(
                subghz->scene_manager, SubGhzSceneStart);
            return true;
        }
        scene_manager_search_and_switch_to_previous_scene(
            subghz->scene_manager, SubGhzSceneStart);
        return true;
    } else if(event.type == SceneManagerEventTypeCustom) {
        uint8_t idx = subghz_scene_brute_force_event_to_index(event.event);
        if(idx != 0xFF) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneBruteForce, event.event);
            subghz_scene_brute_force_start_attack(subghz, idx);
            dolphin_deed(DolphinDeedSubGhzSend);
            return true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(bf_state.is_attacking) {
            if(bf_state.current_key < bf_state.total_keys) {
                // Send a batch of keys per tick
                for(uint8_t i = 0; i < 3 && bf_state.current_key < bf_state.total_keys; i++) {
                    subghz_scene_brute_force_send_next_key(subghz);
                }
                subghz_scene_brute_force_update_progress(subghz);
                notification_message(subghz->notifications, &sequence_blink_magenta_10);
            } else {
                // Done
                bf_state.is_attacking = false;
                widget_reset(subghz->widget);
                widget_add_string_element(
                    subghz->widget, 64, 2, AlignCenter, AlignTop, FontPrimary, "Brute Force");
                widget_add_string_multiline_element(
                    subghz->widget,
                    64,
                    22,
                    AlignCenter,
                    AlignTop,
                    FontSecondary,
                    "Complete!\nAll keys transmitted.\nPress Back to return.");
                notification_message(subghz->notifications, &sequence_success);
            }
            return true;
        }
    }
    return false;
}

void subghz_scene_brute_force_on_exit(void* context) {
    SubGhz* subghz = context;
    bf_state.is_attacking = false;
    subghz->state_notifications = SubGhzNotificationStateIDLE;
    submenu_reset(subghz->submenu);
    widget_reset(subghz->widget);
}
