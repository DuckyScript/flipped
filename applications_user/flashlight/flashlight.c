#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>

#define TAG "Flashlight"

typedef struct {
    FuriMessageQueue* input_queue;
    Gui* gui;
    ViewPort* view_port;
    NotificationApp* notification;
    bool light_on;
} FlashlightApp;

static const NotificationSequence sequence_light_on = {
    &message_red_255,
    &message_green_255,
    &message_blue_255,
    &message_display_backlight_on,
    &message_do_not_reset,
    NULL,
};

static const NotificationSequence sequence_light_off = {
    &message_red_0,
    &message_green_0,
    &message_blue_0,
    NULL,
};

static void flashlight_draw_callback(Canvas* canvas, void* context) {
    furi_assert(context);
    FlashlightApp* app = context;

    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 8, AlignCenter, AlignTop, "Flashlight");

    canvas_set_font(canvas, FontBigNumbers);
    if(app->light_on) {
        canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignTop, "ON");
    } else {
        canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignTop, "OFF");
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 54, AlignCenter, AlignTop, "Press OK to toggle");
}

static void flashlight_input_callback(InputEvent* input_event, void* context) {
    furi_assert(context);
    FlashlightApp* app = context;
    furi_message_queue_put(app->input_queue, input_event, FuriWaitForever);
}

static FlashlightApp* flashlight_app_alloc(void) {
    FlashlightApp* app = malloc(sizeof(FlashlightApp));
    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->light_on = false;

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, flashlight_draw_callback, app);
    view_port_input_callback_set(app->view_port, flashlight_input_callback, app);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    app->notification = furi_record_open(RECORD_NOTIFICATION);

    return app;
}

static void flashlight_app_free(FlashlightApp* app) {
    notification_message(app->notification, &sequence_light_off);
    notification_message(app->notification, &sequence_display_backlight_enforce_auto);

    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_message_queue_free(app->input_queue);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    free(app);
}

int32_t flashlight_app(void* p) {
    UNUSED(p);
    FlashlightApp* app = flashlight_app_alloc();

    InputEvent event;
    while(furi_message_queue_get(app->input_queue, &event, FuriWaitForever) == FuriStatusOk) {
        if(event.type != InputTypeShort) continue;

        if(event.key == InputKeyBack) {
            break;
        } else if(event.key == InputKeyOk) {
            app->light_on = !app->light_on;
            if(app->light_on) {
                notification_message(app->notification, &sequence_light_on);
                notification_message(
                    app->notification, &sequence_display_backlight_enforce_on);
            } else {
                notification_message(app->notification, &sequence_light_off);
                notification_message(
                    app->notification, &sequence_display_backlight_enforce_auto);
            }
            view_port_update(app->view_port);
        }
    }

    flashlight_app_free(app);
    return 0;
}
