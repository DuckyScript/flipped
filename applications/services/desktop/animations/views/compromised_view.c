#include "compromised_view.h"
#include <furi.h>
#include <gui/canvas.h>
#include <gui/view.h>
#include <stdint.h>

#define MATRIX_COLS 16
#define MATRIX_ROWS 8

struct CompromisedView {
    View* view;
    FuriTimer* timer;
    CompromisedViewDoneCallback done_callback;
    void* done_callback_context;
};

typedef struct {
    uint8_t columns[MATRIX_COLS];
    uint32_t frame;
} CompromisedViewModel;

static void compromised_view_draw_callback(Canvas* canvas, void* model_) {
    CompromisedViewModel* model = model_;
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0, 128, 64);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontKeyboard);

    if(model->frame < 30) {
        // Matrix effect
        for(int i = 0; i < MATRIX_COLS; i++) {
            uint8_t y = (model->columns[i] + model->frame) % MATRIX_ROWS;
            for(int j = 0; j < 3; j++) {
                int py = (y - j + MATRIX_ROWS) % MATRIX_ROWS;
                canvas_draw_str(canvas, i * 8, (py + 1) * 8, (model->frame % 2) ? "1" : "0");
            }
        }
    } else {
        // "System Compromised" text
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 25, AlignCenter, AlignCenter, "SYSTEM");
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "COMPROMISED");
        
        // Progress bar
        canvas_draw_frame(canvas, 20, 50, 88, 7);
        uint8_t progress = (model->frame - 30) * 4;
        if(progress > 84) progress = 84;
        canvas_draw_box(canvas, 22, 52, progress, 3);
    }
}

static void compromised_view_timer_callback(void* context) {
    CompromisedView* instance = context;
    with_view_model(
        instance->view,
        CompromisedViewModel * model,
        {
            model->frame++;
            if(model->frame > 60) {
                if(instance->done_callback) {
                    instance->done_callback(instance->done_callback_context);
                }
            }
        },
        true);
}

static bool compromised_view_input_callback(InputEvent* event, void* context) {
    CompromisedView* instance = context;
    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        if(instance->done_callback) {
            instance->done_callback(instance->done_callback_context);
        }
        return true;
    }
    return false;
}

CompromisedView* compromised_view_alloc(void) {
    CompromisedView* instance = malloc(sizeof(CompromisedView));
    instance->view = view_alloc();
    view_allocate_model(instance->view, ViewModelTypeLocking, sizeof(CompromisedViewModel));
    view_set_context(instance->view, instance);
    view_set_draw_callback(instance->view, compromised_view_draw_callback);
    view_set_input_callback(instance->view, compromised_view_input_callback);

    instance->timer = furi_timer_alloc(compromised_view_timer_callback, FuriTimerTypePeriodic, instance);

    with_view_model(
        instance->view,
        CompromisedViewModel * model,
        {
            for(int i = 0; i < MATRIX_COLS; i++) {
                model->columns[i] = furi_hal_random_get() % MATRIX_ROWS;
            }
            model->frame = 0;
        },
        true);

    return instance;
}

void compromised_view_free(CompromisedView* compromised_view) {
    furi_timer_free(compromised_view->timer);
    view_free(compromised_view->view);
    free(compromised_view);
}

View* compromised_view_get_view(CompromisedView* compromised_view) {
    return compromised_view->view;
}

void compromised_view_set_done_callback(CompromisedView* compromised_view, CompromisedViewDoneCallback callback, void* context) {
    compromised_view->done_callback = callback;
    compromised_view->done_callback_context = context;
}

void compromised_view_start(CompromisedView* compromised_view) {
    with_view_model(compromised_view->view, CompromisedViewModel * model, { model->frame = 0; }, true);
    furi_timer_start(compromised_view->timer, 100);
}
