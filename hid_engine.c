#include "hid_engine.h"

#include <math.h>
#include <string.h>

static uint16_t read_u16(const uint8_t *data) {
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

uint32_t hid_read_u32(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static float read_float(const uint8_t *data) {
    uint32_t bits = hid_read_u32(data);
    float value;
    _Static_assert(sizeof(float) == 4, "Protocol requires IEEE-754 binary32");
    memcpy(&value, &bits, sizeof(value));
    return value;
}

bool hid_command_decode(const uint8_t *data, size_t length, HidCommand *out) {
    if (!data || !out || !length) return false;
    HidCommand command = {0};
    command.type = data[0];
    switch (command.type) {
        case CMD_PRESS_KEY:
            if (length != 4 || data[1] < 4 || data[1] > 0xe7) return false;
            command.code = data[1];
            command.hold_ms = read_u16(data + 2);
            if (command.hold_ms > 5000) return false;
            break;
        case CMD_TOGGLE_KEY:
            if (length != 5 || data[1] < 4 || data[1] > 0xe7 || data[2] > 1) return false;
            command.code = data[1]; command.down = data[2] != 0;
            command.hold_ms = read_u16(data + 3);
            if (command.hold_ms > 5000) return false;
            break;
        case CMD_CHORD:
            if (length != 5 || data[2] < 4 || data[2] > 0xe7) return false;
            command.modifier = data[1]; command.code = data[2]; command.hold_ms = read_u16(data + 3);
            if (command.hold_ms > 5000) return false;
            break;
        case CMD_SCROLL:
            if (length != 3) return false;
            command.wheel = (int16_t)read_u16(data + 1);
            break;
        case CMD_MOVE_DIRECT:
        case CMD_MOVE_HUMAN:
            if (length != (command.type == CMD_MOVE_DIRECT ? 5u : 13u)) return false;
            command.dx = (int16_t)read_u16(data + 1);
            command.dy = (int16_t)read_u16(data + 3);
            if (command.type == CMD_MOVE_HUMAN) {
                command.speed = read_float(data + 5);
                command.curvature = read_float(data + 9);
                if (!isfinite(command.speed) || !isfinite(command.curvature) ||
                    command.speed < 0 || command.speed > 100 ||
                    command.curvature < -100 || command.curvature > 100) return false;
            }
            break;
        case CMD_CLICK_BUTTON:
            if ((length != 2 && length != 4) || data[1] == 0 || data[1] > 31) return false;
            command.code = data[1];
            command.hold_ms = length == 4 ? read_u16(data + 2) : 0;
            if (command.hold_ms > 5000) return false;
            break;
        case CMD_TOGGLE_BUTTON:
            if (length != 5 || data[1] == 0 || data[1] > 31 || data[2] > 1) return false;
            command.code = data[1]; command.down = data[2] != 0; command.hold_ms = read_u16(data + 3);
            if (command.hold_ms > 5000) return false;
            break;
        case CMD_RELEASE_ALL:
            if (length != 1) return false;
            break;
        default:
            return false;
    }
    *out = command;
    return true;
}

static void write_u32(uint8_t *out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (i * 8));
}

void hid_health_encode(uint8_t out[HID_HEALTH_SIZE], uint32_t token,
                       uint8_t flags, uint8_t queued, uint32_t uptime_ms,
                       uint32_t accepted, uint32_t rejected) {
    memcpy(out, "PICO", 4);
    out[4] = 2;
    out[5] = flags;
    out[6] = queued;
    out[7] = HID_QUEUE_CAPACITY;
    write_u32(out + 8, token);
    write_u32(out + 12, uptime_ms);
    write_u32(out + 16, accepted);
    write_u32(out + 20, rejected);
}

static uint32_t next_random(HidEngine *engine) {
    uint32_t value = engine->random_state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    return engine->random_state = value;
}

static uint32_t noisy_delay(HidEngine *engine, uint32_t base, uint32_t variance) {
    if (base == 0) return 0;
    int32_t delay = (int32_t)base + (int32_t)(next_random(engine) % (2 * variance + 1)) - (int32_t)variance;
    return (uint32_t)(delay < 1 ? 1 : delay);
}

void hid_engine_init(HidEngine *engine, HidTransport transport, uint32_t seed) {
    memset(engine, 0, sizeof(*engine));
    engine->transport = transport;
    engine->random_state = seed ? seed : 0x9e3779b9u;
}

bool hid_engine_submit(HidEngine *engine, const HidCommand *command) {
    if (engine->count == HID_QUEUE_CAPACITY) {
        ++engine->rejected;
        return false;
    }
    engine->queue[(engine->head + engine->count) % HID_QUEUE_CAPACITY] = *command;
    ++engine->count;
    ++engine->accepted;
    return true;
}

void hid_engine_abort(HidEngine *engine) {
    engine->rejected += engine->count + (engine->active ? 1u : 0u);
    engine->count = 0;
    engine->head = 0;
    engine->active = false;
    engine->held_modifier = engine->held_buttons = 0;
    memset(engine->held_keys, 0, sizeof(engine->held_keys));
    /* A failed/reconnected USB transfer must never leave a key/button held. */
    engine->release_mouse = true;
    engine->release_keyboard = true;
}

bool hid_engine_busy(const HidEngine *engine) {
    return engine->active || engine->count || engine->release_mouse || engine->release_keyboard;
}

static void start_command(HidEngine *engine, uint64_t now_us) {
    engine->command = engine->queue[engine->head];
    engine->head = (uint8_t)((engine->head + 1) % HID_QUEUE_CAPACITY);
    --engine->count;
    engine->active = true;
    engine->phase = 0;
    engine->step = 0;
    engine->sent_x = engine->sent_y = 0;
    engine->sent_wheel = 0;
    engine->target_x = engine->target_y = 0;
    engine->due_us = engine->progress_us = now_us;

    if (engine->command.type != CMD_MOVE_HUMAN) return;
    float dx = (float)engine->command.dx, dy = (float)engine->command.dy;
    float distance = sqrtf(dx * dx + dy * dy);
    int steps = (int)(distance / 2.0f);
    engine->steps = (uint16_t)(steps < 5 ? 5 : steps > 200 ? 200 : steps);
    /* Signed curvature selects the side of the straight start/end vector. */
    float bend = engine->command.curvature / 200.0f;
    engine->control1_x = dx / 3.0f - dy * bend;
    engine->control1_y = dy / 3.0f + dx * bend;
    engine->control2_x = 2.0f * dx / 3.0f - dy * bend;
    engine->control2_y = 2.0f * dy / 3.0f + dx * bend;
}

static int8_t report_part(int32_t value) {
    return (int8_t)(value > 127 ? 127 : value < -127 ? -127 : value);
}

static void tick_movement(HidEngine *engine, uint64_t now_us) {
    bool human = engine->command.type == CMD_MOVE_HUMAN;
    if (engine->command.dx == 0 && engine->command.dy == 0) {
        engine->active = false;
        return;
    }
    if (!human) {
        engine->target_x = engine->command.dx;
        engine->target_y = engine->command.dy;
    } else if (engine->phase == 0) {
        ++engine->step;
        float t = (float)engine->step / engine->steps;
        /* Smooth start/end speed while retaining the cubic Bezier geometry. */
        t = t * t * (3.0f - 2.0f * t);
        float u = 1.0f - t;
        engine->target_x = (int32_t)lroundf(3 * u * u * t * engine->control1_x +
                                         3 * u * t * t * engine->control2_x + t * t * t * engine->command.dx);
        engine->target_y = (int32_t)lroundf(3 * u * u * t * engine->control1_y +
                                         3 * u * t * t * engine->control2_y + t * t * t * engine->command.dy);
        /* Explicit integer final target avoids any accumulated rounding drift. */
        if (engine->step == engine->steps) {
            engine->target_x = engine->command.dx;
            engine->target_y = engine->command.dy;
        }
        engine->phase = 1;
    }

    int8_t dx = report_part(engine->target_x - engine->sent_x);
    int8_t dy = report_part(engine->target_y - engine->sent_y);
    if (dx || dy) {
        if (!engine->transport.mouse(engine->transport.context, engine->held_buttons, dx, dy, 0)) return;
        engine->sent_x += dx;
        engine->sent_y += dy;
        engine->progress_us = now_us;
    }
    if (engine->sent_x != engine->target_x || engine->sent_y != engine->target_y) return;
    if (!human || engine->step == engine->steps) {
        engine->active = false;
        return;
    }
    engine->phase = 0;
    uint32_t delay = 15u - (uint32_t)(13.0f * engine->command.speed / 100.0f);
    if (engine->step * 10u < engine->steps || engine->step * 10u > engine->steps * 9u) delay += 3;
    engine->due_us = now_us + noisy_delay(engine, delay, 1) * 1000u;
    engine->progress_us = engine->due_us;
}

static bool change_key(uint8_t *modifier, uint8_t keys[6], uint8_t key, bool down) {
    if (key >= 0xe0) {
        uint8_t bit = (uint8_t)(1u << (key - 0xe0));
        if (down) *modifier |= bit; else *modifier &= (uint8_t)~bit;
        return true;
    }
    for (unsigned i = 0; i < 6; ++i) {
        if (keys[i] == key) { if (!down) keys[i] = 0; return true; }
    }
    if (!down) return true;
    for (unsigned i = 0; i < 6; ++i) { if (!keys[i]) { keys[i] = key; return true; } }
    return false;
}

void hid_engine_tick(HidEngine *engine, uint64_t now_us, bool available) {
    if (!available) {
        hid_engine_abort(engine);
        return;
    }
    if (engine->release_keyboard || engine->release_mouse) {
        /* The independent endpoints must not starve each other's recovery. */
        const uint8_t empty_keys[6] = {0};
        if (engine->release_keyboard && engine->transport.keyboard(engine->transport.context, 0, empty_keys))
            engine->release_keyboard = false;
        if (engine->release_mouse && engine->transport.mouse(engine->transport.context, 0, 0, 0, 0))
            engine->release_mouse = false;
        return;
    }
    if (!engine->active) {
        if (!engine->count) return;
        start_command(engine, now_us);
    }
    if (now_us < engine->due_us) return;
    if (now_us >= engine->progress_us && now_us - engine->progress_us >= HID_USB_STALL_US) {
        hid_engine_abort(engine);
        return;
    }
    if (engine->command.type == CMD_MOVE_DIRECT || engine->command.type == CMD_MOVE_HUMAN) {
        tick_movement(engine, now_us);
        return;
    }
    if (engine->command.type == CMD_RELEASE_ALL) {
        engine->held_modifier = engine->held_buttons = 0;
        memset(engine->held_keys, 0, sizeof(engine->held_keys));
        engine->release_keyboard = engine->release_mouse = true;
        engine->active = false;
        return;
    }
    if (engine->command.type == CMD_SCROLL) {
        int8_t wheel = report_part((int32_t)engine->command.wheel - engine->sent_wheel);
        if (wheel && !engine->transport.mouse(engine->transport.context, engine->held_buttons, 0, 0, wheel)) return;
        engine->sent_wheel += wheel;
        engine->progress_us = now_us;
        if (engine->sent_wheel == engine->command.wheel) engine->active = false;
        return;
    }
    bool keyboard = engine->command.type == CMD_PRESS_KEY || engine->command.type == CMD_CHORD || engine->command.type == CMD_TOGGLE_KEY;
    bool toggle = engine->command.type == CMD_TOGGLE_KEY || engine->command.type == CMD_TOGGLE_BUTTON;
    if (toggle && engine->phase == 1) { engine->active = false; return; }
    uint8_t modifier = engine->held_modifier, keys[6], buttons = engine->held_buttons;
    memcpy(keys, engine->held_keys, sizeof(keys));
    if (engine->phase == 0) {
        if (keyboard) {
            modifier |= engine->command.modifier;
            if (!change_key(&modifier, keys, engine->command.code, toggle ? engine->command.down : true)) {
                ++engine->rejected; engine->active = false; return;
            }
        } else if (toggle && !engine->command.down) buttons &= (uint8_t)~engine->command.code;
        else buttons |= engine->command.code;
    }
    bool sent = keyboard ? engine->transport.keyboard(engine->transport.context, modifier, keys)
                         : engine->transport.mouse(engine->transport.context, buttons, 0, 0, 0);
    if (!sent) return;
    if (engine->phase == 0) {
        engine->phase = 1;
        if (toggle) {
            if (keyboard) { engine->held_modifier = modifier; memcpy(engine->held_keys, keys, sizeof(keys)); }
            else engine->held_buttons = buttons;
        }
        uint32_t hold = toggle || keyboard || engine->command.hold_ms ? noisy_delay(engine, engine->command.hold_ms, 5) : noisy_delay(engine, 80, 20);
        engine->due_us = now_us + hold * 1000u;
        engine->progress_us = engine->due_us;
    } else {
        engine->active = false;
    }
}
