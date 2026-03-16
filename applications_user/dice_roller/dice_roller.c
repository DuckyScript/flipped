#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>

#define TAG "DiceRoller"

typedef struct {
    FuriMessageQueue* input_queue;
    Gui* gui;
    ViewPort* view_port;
    NotificationApp* notification;
    uint8_t dice_count; // 1-3
    uint8_t dice_sides; // 4,6,8,10,12,20
    uint8_t results[3];
    bool has_rolled;
} DiceApp;

static const uint8_t SIDES_OPTIONS[] = {4, 6, 8, 10, 12, 20};
static const uint8_t SIDES_COUNT = 6;

static uint8_t sides_index = 1; // default d6

static void dice_draw_callback(Canvas* canvas, void* context) {
    furi_assert(context);
    DiceApp* app = context;

    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    char header[32];
    snprintf(header, sizeof(header), "Dice: %dd%d", app->dice_count, app->dice_sides);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, header);

    if(app->has_rolled) {
        canvas_set_font(canvas, FontBigNumbers);
        uint16_t total = 0;
        for(uint8_t i = 0; i < app->dice_count; i++) {
            total += app->results[i];
        }

        if(app->dice_count == 1) {
            char val[8];
            snprintf(val, sizeof(val), "%d", app->results[0]);
            canvas_draw_str_aligned(canvas, 64, 22, AlignCenter, AlignTop, val);
        } else {
            char detail[32];
            if(app->dice_count == 2) {
                snprintf(
                    detail,
                    sizeof(detail),
                    "%d + %d",
                    app->results[0],
                    app->results[1]);
            } else {
                snprintf(
                    detail,
                    sizeof(detail),
                    "%d + %d + %d",
                    app->results[0],
                    app->results[1],
                    app->results[2]);
            }
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str_aligned(canvas, 64, 22, AlignCenter, AlignTop, detail);

            char total_str[8];
            snprintf(total_str, sizeof(total_str), "%d", total);
            canvas_set_font(canvas, FontBigNumbers);
            canvas_draw_str_aligned(canvas, 64, 33, AlignCenter, AlignTop, total_str);
        }
    } else {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignTop, "Press OK to roll!");
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(
        canvas, 64, 56, AlignCenter, AlignTop, "U/D:Dice  L/R:Sides  OK:Roll");
}

static void dice_input_callback(InputEvent* input_event, void* context) {
    furi_assert(context);
    DiceApp* app = context;
    furi_message_queue_put(app->input_queue, input_event, FuriWaitForever);
}

static DiceApp* dice_app_alloc(void) {
    DiceApp* app = malloc(sizeof(DiceApp));
    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->dice_count = 1;
    app->dice_sides = 6;
    app->has_rolled = false;
    memset(app->results, 0, sizeof(app->results));

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, dice_draw_callback, app);
    view_port_input_callback_set(app->view_port, dice_input_callback, app);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    app->notification = furi_record_open(RECORD_NOTIFICATION);

    return app;
}

static void dice_app_free(DiceApp* app) {
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_message_queue_free(app->input_queue);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    free(app);
}

int32_t dice_roller_app(void* p) {
    UNUSED(p);
    DiceApp* app = dice_app_alloc();

    InputEvent event;
    while(furi_message_queue_get(app->input_queue, &event, FuriWaitForever) == FuriStatusOk) {
        if(event.type != InputTypeShort) continue;

        if(event.key == InputKeyBack) {
            break;
        } else if(event.key == InputKeyOk) {
            for(uint8_t i = 0; i < app->dice_count; i++) {
                app->results[i] = (furi_hal_random_get() % app->dice_sides) + 1;
            }
            app->has_rolled = true;
            notification_message(app->notification, &sequence_single_vibro);
        } else if(event.key == InputKeyUp) {
            if(app->dice_count < 3) app->dice_count++;
        } else if(event.key == InputKeyDown) {
            if(app->dice_count > 1) app->dice_count--;
        } else if(event.key == InputKeyRight) {
            sides_index = (sides_index + 1) % SIDES_COUNT;
            app->dice_sides = SIDES_OPTIONS[sides_index];
            app->has_rolled = false;
        } else if(event.key == InputKeyLeft) {
            sides_index = (sides_index + SIDES_COUNT - 1) % SIDES_COUNT;
            app->dice_sides = SIDES_OPTIONS[sides_index];
            app->has_rolled = false;
        }

        view_port_update(app->view_port);
    }

    dice_app_free(app);
    return 0;
}
