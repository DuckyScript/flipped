#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <storage/storage.h>
#include <dialogs/dialogs.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <stdlib.h>
#include <string.h>

#define TAG "CHIP8"

// CHIP-8 specs
#define C8_MEM_SIZE 4096
#define C8_REGS 16
#define C8_STACK_SIZE 16
#define C8_DISPLAY_W 64
#define C8_DISPLAY_H 32
#define C8_KEYS 16
#define C8_PROG_START 0x200
#define C8_FONT_START 0x050

// Flipper display: 128x64, CHIP-8: 64x32 -> 2x scale
#define SCALE 2

// Cycles per frame (~500Hz / 60fps ~ 8-9 opcodes per frame)
#define CYCLES_PER_FRAME 9
#define FRAME_DELAY_MS 16

// CHIP-8 built-in font (0-F, 5 bytes each)
static const uint8_t c8_font[80] = {
    0xF0, 0x90, 0x90, 0x90, 0xF0, // 0
    0x20, 0x60, 0x20, 0x20, 0x70, // 1
    0xF0, 0x10, 0xF0, 0x80, 0xF0, // 2
    0xF0, 0x10, 0xF0, 0x10, 0xF0, // 3
    0x90, 0x90, 0xF0, 0x10, 0x10, // 4
    0xF0, 0x80, 0xF0, 0x10, 0xF0, // 5
    0xF0, 0x80, 0xF0, 0x90, 0xF0, // 6
    0xF0, 0x10, 0x20, 0x40, 0x40, // 7
    0xF0, 0x90, 0xF0, 0x90, 0xF0, // 8
    0xF0, 0x90, 0xF0, 0x10, 0xF0, // 9
    0xF0, 0x90, 0xF0, 0x90, 0x90, // A
    0xE0, 0x90, 0xE0, 0x90, 0xE0, // B
    0xF0, 0x80, 0x80, 0x80, 0xF0, // C
    0xE0, 0x90, 0x90, 0x90, 0xE0, // D
    0xF0, 0x80, 0xF0, 0x80, 0xF0, // E
    0xF0, 0x80, 0xF0, 0x80, 0x80, // F
};

typedef struct {
    uint8_t memory[C8_MEM_SIZE];
    uint8_t V[C8_REGS];           // registers V0-VF
    uint16_t I;                    // index register
    uint16_t pc;                   // program counter
    uint16_t stack[C8_STACK_SIZE];
    uint8_t sp;                    // stack pointer
    uint8_t delay_timer;
    uint8_t sound_timer;
    uint8_t display[C8_DISPLAY_W * C8_DISPLAY_H];
    bool keys[C8_KEYS];
    bool draw_flag;
    bool waiting_for_key;
    uint8_t key_reg;               // register to store key in for Fx0A
    bool running;
} Chip8;

typedef struct {
    Gui* gui;
    Storage* storage;
    DialogsApp* dialogs;
    NotificationApp* notifications;
    ViewPort* view_port;
    FuriMessageQueue* input_queue;
    FuriMutex* mutex;

    Chip8 cpu;
    bool loaded;
    bool paused;
} Chip8App;

// --- CHIP-8 Core ---

static void c8_reset(Chip8* cpu) {
    memset(cpu->memory, 0, C8_MEM_SIZE);
    memset(cpu->V, 0, C8_REGS);
    memset(cpu->stack, 0, sizeof(cpu->stack));
    memset(cpu->display, 0, sizeof(cpu->display));
    memset(cpu->keys, 0, sizeof(cpu->keys));

    // Load font
    memcpy(cpu->memory + C8_FONT_START, c8_font, sizeof(c8_font));

    cpu->I = 0;
    cpu->pc = C8_PROG_START;
    cpu->sp = 0;
    cpu->delay_timer = 0;
    cpu->sound_timer = 0;
    cpu->draw_flag = true;
    cpu->waiting_for_key = false;
    cpu->key_reg = 0;
    cpu->running = false;
}

static bool c8_load_rom(Chip8* cpu, Storage* storage, const char* path) {
    c8_reset(cpu);

    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        FURI_LOG_E(TAG, "Cannot open ROM: %s", path);
        storage_file_free(file);
        return false;
    }

    size_t max_size = C8_MEM_SIZE - C8_PROG_START;
    size_t total = 0;
    uint8_t buf[256];

    while(total < max_size) {
        size_t to_read = sizeof(buf);
        if(total + to_read > max_size) to_read = max_size - total;
        size_t bytes = storage_file_read(file, buf, to_read);
        if(bytes == 0) break;
        memcpy(cpu->memory + C8_PROG_START + total, buf, bytes);
        total += bytes;
    }

    storage_file_close(file);
    storage_file_free(file);

    if(total == 0) {
        FURI_LOG_E(TAG, "Empty ROM");
        return false;
    }

    cpu->running = true;
    FURI_LOG_I(TAG, "Loaded ROM: %zu bytes", total);
    return true;
}

static void c8_cycle(Chip8* cpu) {
    if(!cpu->running) return;

    // Handle key wait (Fx0A)
    if(cpu->waiting_for_key) {
        for(uint8_t i = 0; i < C8_KEYS; i++) {
            if(cpu->keys[i]) {
                cpu->V[cpu->key_reg] = i;
                cpu->waiting_for_key = false;
                break;
            }
        }
        return;
    }

    if(cpu->pc >= C8_MEM_SIZE - 1) {
        cpu->running = false;
        return;
    }

    uint16_t opcode = (cpu->memory[cpu->pc] << 8) | cpu->memory[cpu->pc + 1];
    cpu->pc += 2;

    uint8_t x = (opcode >> 8) & 0x0F;
    uint8_t y = (opcode >> 4) & 0x0F;
    uint8_t n = opcode & 0x0F;
    uint8_t nn = opcode & 0xFF;
    uint16_t nnn = opcode & 0x0FFF;

    switch(opcode & 0xF000) {
    case 0x0000:
        if(opcode == 0x00E0) {
            // CLS
            memset(cpu->display, 0, sizeof(cpu->display));
            cpu->draw_flag = true;
        } else if(opcode == 0x00EE) {
            // RET
            if(cpu->sp > 0) {
                cpu->sp--;
                cpu->pc = cpu->stack[cpu->sp];
            }
        }
        break;

    case 0x1000: // JP nnn
        cpu->pc = nnn;
        break;

    case 0x2000: // CALL nnn
        if(cpu->sp < C8_STACK_SIZE) {
            cpu->stack[cpu->sp] = cpu->pc;
            cpu->sp++;
        }
        cpu->pc = nnn;
        break;

    case 0x3000: // SE Vx, nn
        if(cpu->V[x] == nn) cpu->pc += 2;
        break;

    case 0x4000: // SNE Vx, nn
        if(cpu->V[x] != nn) cpu->pc += 2;
        break;

    case 0x5000: // SE Vx, Vy
        if(cpu->V[x] == cpu->V[y]) cpu->pc += 2;
        break;

    case 0x6000: // LD Vx, nn
        cpu->V[x] = nn;
        break;

    case 0x7000: // ADD Vx, nn
        cpu->V[x] += nn;
        break;

    case 0x8000:
        switch(n) {
        case 0x0: cpu->V[x] = cpu->V[y]; break;
        case 0x1: cpu->V[x] |= cpu->V[y]; cpu->V[0xF] = 0; break;
        case 0x2: cpu->V[x] &= cpu->V[y]; cpu->V[0xF] = 0; break;
        case 0x3: cpu->V[x] ^= cpu->V[y]; cpu->V[0xF] = 0; break;
        case 0x4: {
            uint16_t sum = cpu->V[x] + cpu->V[y];
            cpu->V[x] = sum & 0xFF;
            cpu->V[0xF] = sum > 0xFF ? 1 : 0;
            break;
        }
        case 0x5: {
            uint8_t flag = cpu->V[x] >= cpu->V[y] ? 1 : 0;
            cpu->V[x] -= cpu->V[y];
            cpu->V[0xF] = flag;
            break;
        }
        case 0x6: {
            uint8_t flag = cpu->V[x] & 1;
            cpu->V[x] >>= 1;
            cpu->V[0xF] = flag;
            break;
        }
        case 0x7: {
            uint8_t flag = cpu->V[y] >= cpu->V[x] ? 1 : 0;
            cpu->V[x] = cpu->V[y] - cpu->V[x];
            cpu->V[0xF] = flag;
            break;
        }
        case 0xE: {
            uint8_t flag = (cpu->V[x] >> 7) & 1;
            cpu->V[x] <<= 1;
            cpu->V[0xF] = flag;
            break;
        }
        }
        break;

    case 0x9000: // SNE Vx, Vy
        if(cpu->V[x] != cpu->V[y]) cpu->pc += 2;
        break;

    case 0xA000: // LD I, nnn
        cpu->I = nnn;
        break;

    case 0xB000: // JP V0, nnn
        cpu->pc = nnn + cpu->V[0];
        break;

    case 0xC000: // RND Vx, nn
        cpu->V[x] = (rand() % 256) & nn;
        break;

    case 0xD000: { // DRW Vx, Vy, n
        uint8_t xpos = cpu->V[x] % C8_DISPLAY_W;
        uint8_t ypos = cpu->V[y] % C8_DISPLAY_H;
        cpu->V[0xF] = 0;

        for(uint8_t row = 0; row < n; row++) {
            if(ypos + row >= C8_DISPLAY_H) break;
            uint8_t sprite = cpu->memory[cpu->I + row];
            for(uint8_t col = 0; col < 8; col++) {
                if(xpos + col >= C8_DISPLAY_W) break;
                if(sprite & (0x80 >> col)) {
                    size_t idx = (ypos + row) * C8_DISPLAY_W + (xpos + col);
                    if(cpu->display[idx]) {
                        cpu->V[0xF] = 1;
                    }
                    cpu->display[idx] ^= 1;
                }
            }
        }
        cpu->draw_flag = true;
        break;
    }

    case 0xE000:
        if(nn == 0x9E) {
            // SKP Vx
            if(cpu->V[x] < C8_KEYS && cpu->keys[cpu->V[x]]) cpu->pc += 2;
        } else if(nn == 0xA1) {
            // SKNP Vx
            if(cpu->V[x] >= C8_KEYS || !cpu->keys[cpu->V[x]]) cpu->pc += 2;
        }
        break;

    case 0xF000:
        switch(nn) {
        case 0x07: cpu->V[x] = cpu->delay_timer; break;
        case 0x0A:
            cpu->waiting_for_key = true;
            cpu->key_reg = x;
            break;
        case 0x15: cpu->delay_timer = cpu->V[x]; break;
        case 0x18: cpu->sound_timer = cpu->V[x]; break;
        case 0x1E: cpu->I += cpu->V[x]; break;
        case 0x29: cpu->I = C8_FONT_START + (cpu->V[x] & 0x0F) * 5; break;
        case 0x33:
            if(cpu->I + 2 < C8_MEM_SIZE) {
                cpu->memory[cpu->I] = cpu->V[x] / 100;
                cpu->memory[cpu->I + 1] = (cpu->V[x] / 10) % 10;
                cpu->memory[cpu->I + 2] = cpu->V[x] % 10;
            }
            break;
        case 0x55:
            for(uint8_t i = 0; i <= x && cpu->I + i < C8_MEM_SIZE; i++) {
                cpu->memory[cpu->I + i] = cpu->V[i];
            }
            break;
        case 0x65:
            for(uint8_t i = 0; i <= x && cpu->I + i < C8_MEM_SIZE; i++) {
                cpu->V[i] = cpu->memory[cpu->I + i];
            }
            break;
        }
        break;
    }
}

static void c8_tick_timers(Chip8* cpu) {
    if(cpu->delay_timer > 0) cpu->delay_timer--;
    if(cpu->sound_timer > 0) cpu->sound_timer--;
}

// --- Input Mapping ---
// CHIP-8 has a 16-key hex pad. Flipper has 5 buttons.
// Map: Up=2, Down=8, Left=4, Right=6, OK=5
// Hold OK + direction for more keys: OK+Up=A, OK+Down=B, OK+Left=C, OK+Right=D

static void c8_clear_keys(Chip8* cpu) {
    memset(cpu->keys, 0, sizeof(cpu->keys));
}

// --- Draw ---

static void chip8_draw_callback(Canvas* canvas, void* context) {
    Chip8App* app = context;

    furi_mutex_acquire(app->mutex, FuriWaitForever);

    canvas_clear(canvas);

    if(!app->loaded) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignCenter, "CHIP-8 Emulator");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter, "Press OK to load ROM");
        canvas_draw_str_aligned(
            canvas, 64, 48, AlignCenter, AlignCenter, "ROMs: /ext/chip8/*.ch8");
        furi_mutex_release(app->mutex);
        return;
    }

    if(app->paused) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 4, AlignCenter, AlignTop, "PAUSED - OK resume");
    }

    // Draw CHIP-8 display scaled 2x
    canvas_set_color(canvas, ColorBlack);
    for(int cy = 0; cy < C8_DISPLAY_H; cy++) {
        for(int cx = 0; cx < C8_DISPLAY_W; cx++) {
            if(app->cpu.display[cy * C8_DISPLAY_W + cx]) {
                canvas_draw_box(canvas, cx * SCALE, cy * SCALE, SCALE, SCALE);
            }
        }
    }

    furi_mutex_release(app->mutex);
}

static void chip8_input_callback(InputEvent* input_event, void* context) {
    Chip8App* app = context;
    furi_message_queue_put(app->input_queue, input_event, FuriWaitForever);
}

// --- App ---

static bool chip8_load_rom_dialog(Chip8App* app) {
    FuriString* rom_path = furi_string_alloc_set_str(EXT_PATH("chip8"));
    FuriString* selected = furi_string_alloc();

    // Create chip8 directory if needed
    storage_simply_mkdir(app->storage, EXT_PATH("chip8"));

    DialogsFileBrowserOptions options;
    dialog_file_browser_set_basic_options(&options, ".ch8", NULL);
    options.base_path = EXT_PATH("chip8");
    options.hide_dot_files = true;

    bool result = dialog_file_browser_show(app->dialogs, selected, rom_path, &options);

    if(result) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        result = c8_load_rom(&app->cpu, app->storage, furi_string_get_cstr(selected));
        app->loaded = result;
        app->paused = false;
        furi_mutex_release(app->mutex);

        if(result) {
            FURI_LOG_I(TAG, "ROM loaded: %s", furi_string_get_cstr(selected));
        }
    }

    furi_string_free(rom_path);
    furi_string_free(selected);
    return result;
}

int32_t chip8_app(void* p) {
    UNUSED(p);

    Chip8App* app = malloc(sizeof(Chip8App));
    memset(app, 0, sizeof(Chip8App));

    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, chip8_draw_callback, app);
    view_port_input_callback_set(app->view_port, chip8_input_callback, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    c8_reset(&app->cpu);
    srand(furi_get_tick());

    bool running = true;

    while(running) {
        InputEvent event;

        // Process input
        if(furi_message_queue_get(app->input_queue, &event, app->loaded ? FRAME_DELAY_MS : 100) ==
           FuriStatusOk) {
            if(event.type == InputTypeLong && event.key == InputKeyBack) {
                // Long back = exit
                running = false;
                continue;
            }

            if(!app->loaded) {
                // Not loaded: OK opens file browser
                if(event.type == InputTypeShort && event.key == InputKeyOk) {
                    chip8_load_rom_dialog(app);
                } else if(event.type == InputTypeShort && event.key == InputKeyBack) {
                    running = false;
                }
                continue;
            }

            // Short back = pause/unpause
            if(event.type == InputTypeShort && event.key == InputKeyBack) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                if(app->paused) {
                    app->paused = false;
                } else {
                    app->paused = true;
                    // While paused, OK loads new ROM
                }
                furi_mutex_release(app->mutex);
                view_port_update(app->view_port);
                continue;
            }

            if(app->paused) {
                if(event.type == InputTypeShort && event.key == InputKeyOk) {
                    furi_mutex_acquire(app->mutex, FuriWaitForever);
                    app->paused = false;
                    furi_mutex_release(app->mutex);
                }
                continue;
            }

            // Map Flipper keys to CHIP-8 keys
            // Layout: Up=2, Down=8, Left=4, Right=6, OK=5
            // CHIP-8 keypad:
            // 1 2 3 C
            // 4 5 6 D
            // 7 8 9 E
            // A 0 B F
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            bool pressed = (event.type == InputTypePress || event.type == InputTypeRepeat);
            bool released = (event.type == InputTypeRelease);

            if(pressed || released) {
                bool state = pressed;
                switch(event.key) {
                case InputKeyUp:
                    app->cpu.keys[0x2] = state;
                    break;
                case InputKeyDown:
                    app->cpu.keys[0x8] = state;
                    break;
                case InputKeyLeft:
                    app->cpu.keys[0x4] = state;
                    break;
                case InputKeyRight:
                    app->cpu.keys[0x6] = state;
                    break;
                case InputKeyOk:
                    app->cpu.keys[0x5] = state;
                    break;
                default:
                    break;
                }
            }
            furi_mutex_release(app->mutex);
        }

        // Run emulator cycles
        if(app->loaded && !app->paused) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);

            if(app->cpu.running) {
                for(int i = 0; i < CYCLES_PER_FRAME; i++) {
                    c8_cycle(&app->cpu);
                }
                c8_tick_timers(&app->cpu);

                if(app->cpu.sound_timer > 0) {
                    notification_message(app->notifications, &sequence_audiovisual_alert);
                }
            }

            bool need_draw = app->cpu.draw_flag;
            app->cpu.draw_flag = false;
            furi_mutex_release(app->mutex);

            if(need_draw) {
                view_port_update(app->view_port);
            }
        }
    }

    // Cleanup
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_message_queue_free(app->input_queue);
    furi_mutex_free(app->mutex);

    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_NOTIFICATION);

    free(app);
    return 0;
}
