#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>

#define TAG "Stopwatch"

typedef struct {
    FuriMessageQueue* input_queue;
    Gui* gui;
    ViewPort* view_port;
    FuriMutex* mutex;
    bool running;
    uint32_t elapsed_ms;
    uint32_t start_tick;
} StopwatchApp;

static void stopwatch_draw_callback(Canvas* canvas, void* context) {
    furi_assert(context);
    StopwatchApp* app = context;

    furi_mutex_acquire(app->mutex, FuriWaitForever);

    uint32_t total_ms = app->elapsed_ms;
    if(app->running) {
        total_ms += (furi_get_tick() - app->start_tick) * 1000 / furi_kernel_get_tick_frequency();
    }

    uint32_t minutes = total_ms / 60000;
    uint32_t seconds = (total_ms / 1000) % 60;
    uint32_t centis = (total_ms / 10) % 100;

    furi_mutex_release(app->mutex);

    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 8, AlignCenter, AlignTop, "Stopwatch");

    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%02lu:%02lu.%02lu", minutes, seconds, centis);
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignTop, time_str);

    canvas_set_font(canvas, FontSecondary);
    if(app->running) {
        canvas_draw_str_aligned(canvas, 64, 54, AlignCenter, AlignTop, "OK:Stop  Long-OK:Reset");
    } else if(total_ms > 0) {
        canvas_draw_str_aligned(canvas, 64, 54, AlignCenter, AlignTop, "OK:Start  Long-OK:Reset");
    } else {
        canvas_draw_str_aligned(canvas, 64, 54, AlignCenter, AlignTop, "Press OK to start");
    }
}

static void stopwatch_input_callback(InputEvent* input_event, void* context) {
    furi_assert(context);
    StopwatchApp* app = context;
    furi_message_queue_put(app->input_queue, input_event, FuriWaitForever);
}

static StopwatchApp* stopwatch_app_alloc(void) {
    StopwatchApp* app = malloc(sizeof(StopwatchApp));
    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->running = false;
    app->elapsed_ms = 0;
    app->start_tick = 0;

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, stopwatch_draw_callback, app);
    view_port_input_callback_set(app->view_port, stopwatch_input_callback, app);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    return app;
}

static void stopwatch_app_free(StopwatchApp* app) {
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_message_queue_free(app->input_queue);
    furi_mutex_free(app->mutex);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t stopwatch_app(void* p) {
    UNUSED(p);
    StopwatchApp* app = stopwatch_app_alloc();

    InputEvent event;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(app->input_queue, &event, 50) == FuriStatusOk) {
            if(event.key == InputKeyBack && event.type == InputTypeShort) {
                running = false;
            } else if(event.key == InputKeyOk && event.type == InputTypeShort) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                if(app->running) {
                    app->elapsed_ms += (furi_get_tick() - app->start_tick) * 1000 /
                                       furi_kernel_get_tick_frequency();
                    app->running = false;
                } else {
                    app->start_tick = furi_get_tick();
                    app->running = true;
                }
                furi_mutex_release(app->mutex);
            } else if(event.key == InputKeyOk && event.type == InputTypeLong) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                app->running = false;
                app->elapsed_ms = 0;
                furi_mutex_release(app->mutex);
            }
        }
        view_port_update(app->view_port);
    }

    stopwatch_app_free(app);
    return 0;
}
