#ifndef HID_ENGINE_H
#define HID_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HID_QUEUE_CAPACITY 8
#define HID_MAX_PACKET_SIZE 13
#define HID_HEALTH_SIZE 24
#define HID_USB_STALL_US 2000000u

enum {
    CMD_PING = 0,
    CMD_PRESS_KEY = 1,
    CMD_TOGGLE_KEY = 2,
    CMD_MOVE_DIRECT = 3,
    CMD_MOVE_HUMAN = 4,
    CMD_CHORD = 5,
    CMD_SCROLL = 6,
    CMD_CLICK_BUTTON = 8,
    CMD_TOGGLE_BUTTON = 9,
    CMD_RELEASE_ALL = 10
};

typedef struct {
    uint8_t type;
    uint8_t code;
    uint8_t modifier;
    bool down;
    uint16_t hold_ms;
    int16_t dx, dy, wheel;
    float speed, curvature;
} HidCommand;

/* Hooks return true only when the USB stack accepted the report. */
typedef struct {
    bool (*mouse)(void *context, uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);
    bool (*keyboard)(void *context, uint8_t modifier, const uint8_t keys[6]);
    void *context;
} HidTransport;

typedef struct {
    HidTransport transport;
    HidCommand queue[HID_QUEUE_CAPACITY];
    HidCommand command;
    uint8_t head, count, phase;
    bool active, release_mouse, release_keyboard;
    uint8_t held_modifier, held_keys[6], held_buttons;
    uint16_t step, steps;
    int32_t sent_x, sent_y, target_x, target_y;
    int32_t sent_wheel;
    float control1_x, control1_y, control2_x, control2_y;
    uint64_t due_us, progress_us;
    uint32_t random_state, accepted, rejected;
} HidEngine;

bool hid_command_decode(const uint8_t *data, size_t length, HidCommand *out);
uint32_t hid_read_u32(const uint8_t *data);
void hid_health_encode(uint8_t out[HID_HEALTH_SIZE], uint32_t token,
                       uint8_t flags, uint8_t queued, uint32_t uptime_ms,
                       uint32_t accepted, uint32_t rejected);
void hid_engine_init(HidEngine *engine, HidTransport transport, uint32_t seed);
bool hid_engine_submit(HidEngine *engine, const HidCommand *command);
void hid_engine_abort(HidEngine *engine);
/* Call frequently from the owning USB core's application loop, never an IRQ. */
void hid_engine_tick(HidEngine *engine, uint64_t now_us, bool available);
bool hid_engine_busy(const HidEngine *engine);

#endif
