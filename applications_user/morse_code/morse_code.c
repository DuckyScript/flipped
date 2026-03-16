#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>

#define TAG "MorseCode"

#define MORSE_MAX_INPUT 32
#define MORSE_MAX_OUTPUT 128

typedef struct {
    FuriMessageQueue* input_queue;
    Gui* gui;
    ViewPort* view_port;
    NotificationApp* notification;
    char input_text[MORSE_MAX_INPUT];
    char morse_output[MORSE_MAX_OUTPUT];
    uint8_t cursor;
    bool playing;
} MorseApp;

// clang-format off
static const char* morse_table[] = {
    ".-",   "-...", "-.-.", "-..",  ".",    "..-.", "--.",  // A-G
    "....", "..",   ".---", "-.-",  ".-..", "--",   "-.",   // H-N
    "---",  ".--.", "--.-", ".-.",  "...",  "-",    "..-",  // O-U
    "...-", ".--", "-..-", "-.--", "--..",                  // V-Z
};

static const char* morse_digits[] = {
    "-----", ".----", "..---", "...--", "....-",           // 0-4
    ".....", "-....", "--...", "---..", "----.",             // 5-9
};
// clang-format on

static const char* char_to_morse(char c) {
    if(c >= 'A' && c <= 'Z') return morse_table[c - 'A'];
    if(c >= 'a' && c <= 'z') return morse_table[c - 'a'];
    if(c >= '0' && c <= '9') return morse_digits[c - '0'];
    return NULL;
}

static void morse_build_output(MorseApp* app) {
    app->morse_output[0] = '\0';
    size_t pos = 0;

    for(uint8_t i = 0; i < app->cursor && pos < MORSE_MAX_OUTPUT - 8; i++) {
        if(app->input_text[i] == ' ') {
            if(pos + 3 < MORSE_MAX_OUTPUT) {
                app->morse_output[pos++] = ' ';
                app->morse_output[pos++] = '/';
                app->morse_output[pos++] = ' ';
            }
        } else {
            const char* code = char_to_morse(app->input_text[i]);
            if(code) {
                if(i > 0 && app->input_text[i - 1] != ' ' && pos > 0) {
                    app->morse_output[pos++] = ' ';
                }
                size_t len = strlen(code);
                if(pos + len < MORSE_MAX_OUTPUT) {
                    memcpy(&app->morse_output[pos], code, len);
                    pos += len;
                }
            }
        }
    }
    app->morse_output[pos] = '\0';
}

static void morse_draw_callback(Canvas* canvas, void* context) {
    furi_assert(context);
    MorseApp* app = context;

    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, "Morse Code");

    canvas_set_font(canvas, FontSecondary);

    // Show input text
    char display[MORSE_MAX_INPUT + 2];
    snprintf(display, sizeof(display), "> %s_", app->input_text);
    canvas_draw_str(canvas, 2, 22, display);

    // Show morse output (scroll if needed)
    if(app->morse_output[0]) {
        size_t len = strlen(app->morse_output);
        if(len > 20) {
            canvas_draw_str(canvas, 2, 36, &app->morse_output[len - 20]);
        } else {
            canvas_draw_str(canvas, 2, 36, app->morse_output);
        }
    }

    canvas_draw_str(canvas, 2, 52, "U/D:Char L/R:Move OK:Space");
    canvas_draw_str(canvas, 2, 62, "Long-OK:Play  Back:Del/Exit");
}

static void morse_input_callback(InputEvent* input_event, void* context) {
    furi_assert(context);
    MorseApp* app = context;
    furi_message_queue_put(app->input_queue, input_event, FuriWaitForever);
}

static void morse_play_sequence(MorseApp* app) {
    const uint32_t dot_ms = 100;
    const uint32_t dash_ms = 300;
    const uint32_t gap_ms = 100;
    const uint32_t char_gap_ms = 300;
    const uint32_t word_gap_ms = 700;

    const NotificationSequence seq_beep_short = {
        &message_note_c7,
        &message_delay_100,
        &message_sound_off,
        NULL,
    };
    const NotificationSequence seq_beep_long = {
        &message_note_c7,
        &message_delay_100,
        &message_delay_100,
        &message_delay_100,
        &message_sound_off,
        NULL,
    };

    for(uint8_t i = 0; i < app->cursor; i++) {
        if(app->input_text[i] == ' ') {
            furi_delay_ms(word_gap_ms);
            continue;
        }

        const char* code = char_to_morse(app->input_text[i]);
        if(!code) continue;

        for(size_t j = 0; code[j]; j++) {
            if(code[j] == '.') {
                notification_message_block(app->notification, &seq_beep_short);
                furi_delay_ms(dot_ms); // extra gap
            } else if(code[j] == '-') {
                notification_message_block(app->notification, &seq_beep_long);
                furi_delay_ms(dash_ms - 300 + gap_ms); // already played 300ms
            }
        }
        furi_delay_ms(char_gap_ms);
    }
}

static MorseApp* morse_app_alloc(void) {
    MorseApp* app = malloc(sizeof(MorseApp));
    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->cursor = 0;
    app->playing = false;
    memset(app->input_text, 0, sizeof(app->input_text));
    app->morse_output[0] = '\0';

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, morse_draw_callback, app);
    view_port_input_callback_set(app->view_port, morse_input_callback, app);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    app->notification = furi_record_open(RECORD_NOTIFICATION);

    return app;
}

static void morse_app_free(MorseApp* app) {
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_message_queue_free(app->input_queue);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    free(app);
}

int32_t morse_code_app(void* p) {
    UNUSED(p);
    MorseApp* app = morse_app_alloc();

    // Start with 'A'
    char current_char = 'A';

    InputEvent event;
    while(furi_message_queue_get(app->input_queue, &event, FuriWaitForever) == FuriStatusOk) {
        if(event.key == InputKeyBack && event.type == InputTypeShort) {
            if(app->cursor > 0) {
                app->cursor--;
                app->input_text[app->cursor] = '\0';
                morse_build_output(app);
                view_port_update(app->view_port);
            } else {
                break;
            }
        } else if(event.key == InputKeyBack && event.type == InputTypeLong) {
            break;
        } else if(event.type == InputTypeShort || event.type == InputTypeRepeat) {
            if(event.key == InputKeyUp) {
                if(current_char == 'Z')
                    current_char = '0';
                else if(current_char == '9')
                    current_char = 'A';
                else
                    current_char++;

                if(app->cursor < MORSE_MAX_INPUT - 1) {
                    app->input_text[app->cursor] = current_char;
                    app->input_text[app->cursor + 1] = '\0';
                    morse_build_output(app);
                }
            } else if(event.key == InputKeyDown) {
                if(current_char == 'A')
                    current_char = '9';
                else if(current_char == '0')
                    current_char = 'Z';
                else
                    current_char--;

                if(app->cursor < MORSE_MAX_INPUT - 1) {
                    app->input_text[app->cursor] = current_char;
                    app->input_text[app->cursor + 1] = '\0';
                    morse_build_output(app);
                }
            } else if(event.key == InputKeyRight) {
                if(app->cursor < MORSE_MAX_INPUT - 1) {
                    if(app->input_text[app->cursor] == '\0') {
                        app->input_text[app->cursor] = current_char;
                    }
                    app->cursor++;
                    app->input_text[app->cursor] = '\0';
                    current_char = 'A';
                    morse_build_output(app);
                }
            } else if(event.key == InputKeyOk && event.type == InputTypeShort) {
                if(app->cursor < MORSE_MAX_INPUT - 2) {
                    if(app->input_text[app->cursor] == '\0') {
                        app->input_text[app->cursor] = current_char;
                    }
                    app->cursor++;
                    app->input_text[app->cursor] = ' ';
                    app->cursor++;
                    app->input_text[app->cursor] = '\0';
                    current_char = 'A';
                    morse_build_output(app);
                }
            }
        }

        if(event.key == InputKeyOk && event.type == InputTypeLong) {
            if(app->cursor > 0) {
                morse_play_sequence(app);
            }
        }

        view_port_update(app->view_port);
    }

    morse_app_free(app);
    return 0;
}
