#include <furi.h>
#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/text_input.h>
#include <targets/furi_hal_include/furi_hal_version.h>

#include "desktop_settings_scene.h"
#include "../desktop_settings_app.h"

void desktop_settings_scene_device_name_text_input_callback(void* context) {
    DesktopSettingsApp* app = context;
    furi_hal_version_set_custom_name(app->text_store);
    scene_manager_previous_scene(app->scene_manager);
}

void desktop_settings_scene_device_name_on_enter(void* context) {
    DesktopSettingsApp* app = context;
    const char* current_name = furi_hal_version_get_name_ptr();

    if(current_name) {
        strlcpy(app->text_store, current_name, sizeof(app->text_store));
    } else {
        app->text_store[0] = '\0';
    }

    text_input_set_header_text(app->text_input, "Enter Device Name");
    text_input_set_result_callback(
        app->text_input,
        desktop_settings_scene_device_name_text_input_callback,
        app,
        app->text_store,
        FURI_HAL_VERSION_ARRAY_NAME_LENGTH,
        true);

    view_dispatcher_switch_to_view(app->view_dispatcher, DesktopSettingsAppViewTextInput);
}

bool desktop_settings_scene_device_name_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void desktop_settings_scene_device_name_on_exit(void* context) {
    DesktopSettingsApp* app = context;
    text_input_reset(app->text_input);
}
