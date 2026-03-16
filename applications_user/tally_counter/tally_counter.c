#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>

#define TAG "TallyCounter"

typedef struct {
    FuriMessageQueue* input_queue;
    Gui* gui;
    ViewPort* view_port;
    NotificationApp* notification;
    int32_t count;
    int32_t step;
} TallyApp;

static void tally_draw_callback(Canvas* canvas, void* context) {
    furi_assert(context);
    TallyApp* app = context;

    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, "Tally Counter");

    char count_str[16];
    snprintf(count_str, sizeof(count_str), "%ld", app->count);
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignTop, count_str);

    canvas_set_font(canvas, FontSecondary);
    char step_str[24];
    snprintf(step_str, sizeof(step_str), "Step: %ld", app->step);
    canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignTop, step_str);

    canvas_draw_str_aligned(
        canvas, 64, 56, AlignCenter, AlignTop, "U:+  D:-  L/R:Step  OK:Reset");
}

static void tally_input_callback(InputEvent* input_event, void* context) {
    furi_assert(context);
    TallyApp* app = context;
    furi_message_queue_put(app->input_queue, input_event, FuriWaitForever);
}

static TallyApp* tally_app_alloc(void) {
    TallyApp* app = malloc(sizeof(TallyApp));
    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->count = 0;
    app->step = 1;

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, tally_draw_callback, app);
    view_port_input_callback_set(app->view_port, tally_input_callback, app);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    app->notification = furi_record_open(RECORD_NOTIFICATION);

    return app;
}

static void tally_app_free(TallyApp* app) {
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_message_queue_free(app->input_queue);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    free(app);
}

int32_t tally_counter_app(void* p) {
    UNUSED(p);
    TallyApp* app = tally_app_alloc();

    InputEvent event;
    while(furi_message_queue_get(app->input_queue, &event, FuriWaitForever) == FuriStatusOk) {
        if(event.type != InputTypeShort && event.type != InputTypeRepeat) continue;

        if(event.key == InputKeyBack) {
            break;
        } else if(event.key == InputKeyUp) {
            app->count += app->step;
            notification_message(app->notification, &sequence_single_vibro);
        } else if(event.key == InputKeyDown) {
            app->count -= app->step;
            notification_message(app->notification, &sequence_single_vibro);
        } else if(event.key == InputKeyRight) {
            if(app->step < 100) app->step *= 10;
        } else if(event.key == InputKeyLeft) {
            if(app->step > 1) app->step /= 10;
        } else if(event.key == InputKeyOk) {
            app->count = 0;
        }

        view_port_update(app->view_port);
    }

    tally_app_free(app);
    return 0;
}
