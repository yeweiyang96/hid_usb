#include "hid_engine.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(float) == sizeof(uint32_t), "Protocol floats must occupy four bytes");
_Static_assert(HID_HEALTH_SIZE == 24, "Health protocol must occupy 24 bytes");

/* Runs the production engine with a fake USB transport and a deterministic clock. */
typedef struct {
    uint64_t at_us;
    uint8_t kind, code, modifier;
    int8_t dx, dy;
} Report;

typedef struct {
    HidEngine engine;
    bool accept_mouse, accept_keyboard;
    uint64_t now_us;
    uint32_t mouse_attempts, keyboard_attempts;
    int32_t total_x, total_y;
    size_t report_count;
    Report reports[32768];
} Fixture;

static Fixture fixture;

static void fail(const char *expression, const char *file, int line) {
    fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
    exit(1);
}

#define CHECK(expression) do { if (!(expression)) fail(#expression, __FILE__, __LINE__); } while (0)

static Report *append_report(Fixture *f, uint8_t kind) {
    CHECK(f->report_count < sizeof(f->reports) / sizeof(f->reports[0]));
    Report *report = &f->reports[f->report_count++];
    report->kind = kind;
    report->at_us = f->now_us;
    return report;
}

static bool send_mouse(void *context, uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel) {
    CHECK(wheel == 0);
    Fixture *f = (Fixture *)context;
    ++f->mouse_attempts;
    CHECK(dx >= -127 && dy >= -127);
    if (!f->accept_mouse) return false;
    Report *report = append_report(f, 1);
    report->code = buttons;
    report->dx = dx;
    report->dy = dy;
    f->total_x += dx;
    f->total_y += dy;
    return true;
}

static bool send_keyboard(void *context, uint8_t modifier, const uint8_t keys[6]) {
    Fixture *f = (Fixture *)context;
    ++f->keyboard_attempts;
    if (!f->accept_keyboard) return false;
    Report *report = append_report(f, 2);
    report->modifier = modifier;
    report->code = keys[0];
    return true;
}

static void reset_fixture(uint32_t seed) {
    memset(&fixture, 0, sizeof(fixture));
    fixture.accept_mouse = true;
    fixture.accept_keyboard = true;
    fixture.now_us = 1000000;
    HidTransport transport = {send_mouse, send_keyboard, &fixture};
    hid_engine_init(&fixture.engine, transport, seed);
    CHECK(!hid_engine_busy(&fixture.engine));
    CHECK(!fixture.engine.release_mouse && !fixture.engine.release_keyboard);
}

static void tick_at(uint64_t now_us, bool available) {
    fixture.now_us = now_us;
    hid_engine_tick(&fixture.engine, now_us, available);
}

static void tick_after(uint64_t delay_us) {
    tick_at(fixture.now_us + delay_us, true);
}

static void drain(void) {
    /* Allows slow human motion while detecting a stuck queue or report retry. */
    for (uint32_t i = 0; i < 2000000 && hid_engine_busy(&fixture.engine); ++i) {
        tick_after(1000);
    }
    CHECK(!hid_engine_busy(&fixture.engine));
}

static void put_u16(uint8_t *data, uint16_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void put_float(uint8_t *data, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    data[0] = (uint8_t)bits;
    data[1] = (uint8_t)(bits >> 8);
    data[2] = (uint8_t)(bits >> 16);
    data[3] = (uint8_t)(bits >> 24);
}

static void check_exact_length(uint8_t *packet, size_t length) {
    HidCommand command;
    CHECK(hid_command_decode(packet, length, &command));
    for (size_t shorter = 0; shorter < length; ++shorter) {
        CHECK(!hid_command_decode(packet, shorter, &command));
    }
    CHECK(!hid_command_decode(packet, length + 1, &command));
}

static void test_decode(void) {
    uint8_t packet[HID_MAX_PACKET_SIZE + 1] = {0};
    HidCommand command;
    packet[0] = CMD_PRESS_KEY;
    packet[1] = 4;
    put_u16(packet + 2, 5000);
    check_exact_length(packet, 4);
    CHECK(hid_command_decode(packet, 4, &command));
    CHECK(command.type == CMD_PRESS_KEY && command.code == 4 && command.hold_ms == 5000);
    packet[1] = 0xE7;
    CHECK(hid_command_decode(packet, 4, &command));
    packet[1] = 3;
    CHECK(!hid_command_decode(packet, 4, &command));
    packet[1] = 0xE8;
    CHECK(!hid_command_decode(packet, 4, &command));
    packet[1] = 4;
    put_u16(packet + 2, 5001);
    CHECK(!hid_command_decode(packet, 4, &command));

    packet[0] = CMD_MOVE_DIRECT;
    put_u16(packet + 1, (uint16_t)-32768);
    put_u16(packet + 3, 32767);
    check_exact_length(packet, 5);
    CHECK(hid_command_decode(packet, 5, &command));
    CHECK(command.dx == -32768 && command.dy == 32767);

    packet[0] = CMD_MOVE_HUMAN;
    put_float(packet + 5, 0.0f);
    put_float(packet + 9, -100.0f);
    check_exact_length(packet, 13);
    CHECK(hid_command_decode(packet, 13, &command));
    CHECK(command.dx == -32768 && command.dy == 32767);
    CHECK(command.speed == 0.0f && command.curvature == -100.0f);
    put_float(packet + 5, 100.0f);
    put_float(packet + 9, 100.0f);
    CHECK(hid_command_decode(packet, 13, &command));
    const float invalid_speed[] = {-0.001f, 100.001f, NAN, INFINITY, -INFINITY};
    for (size_t i = 0; i < sizeof(invalid_speed) / sizeof(invalid_speed[0]); ++i) {
        put_float(packet + 5, invalid_speed[i]);
        CHECK(!hid_command_decode(packet, 13, &command));
    }
    put_float(packet + 5, 50.0f);
    const float invalid_curve[] = {-100.001f, 100.001f, NAN, INFINITY, -INFINITY};
    for (size_t i = 0; i < sizeof(invalid_curve) / sizeof(invalid_curve[0]); ++i) {
        put_float(packet + 9, invalid_curve[i]);
        CHECK(!hid_command_decode(packet, 13, &command));
    }

    packet[0] = CMD_CLICK_BUTTON;
    packet[1] = 1;
    check_exact_length(packet, 2);
    packet[1] = 31;
    CHECK(hid_command_decode(packet, 2, &command));
    packet[1] = 0;
    CHECK(!hid_command_decode(packet, 2, &command));
    packet[1] = 32;
    CHECK(!hid_command_decode(packet, 2, &command));
    packet[0] = CMD_PING;
    CHECK(!hid_command_decode(packet, 1, &command));
    packet[0] = 0xFF;
    CHECK(!hid_command_decode(packet, 1, &command));
}

static uint32_t next_random(uint32_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static void check_motion(uint8_t type, int16_t dx, int16_t dy, float speed,
                         float curvature, uint32_t seed) {
    reset_fixture(seed);
    HidCommand command = {0};
    command.type = type;
    command.dx = dx;
    command.dy = dy;
    command.speed = speed;
    command.curvature = curvature;
    CHECK(hid_engine_submit(&fixture.engine, &command));
    drain();
    CHECK(fixture.total_x == dx && fixture.total_y == dy);
    CHECK(fixture.engine.count == 0);
    for (size_t i = 0; i < fixture.report_count; ++i) {
        CHECK(fixture.reports[i].kind == 1 && fixture.reports[i].code == 0);
    }
}

static void test_motion(void) {
    const int16_t coordinates[][2] = {
        {0, 0}, {1, 0}, {0, -1}, {-1, 1}, {127, -127}, {128, -128},
        {-32768, -32768}, {32767, 32767}, {-32768, 32767}, {32767, -32768},
        {0, -32768}, {-32768, 0}, {127, 32767}, {-32768, -1}
    };
    for (size_t i = 0; i < sizeof(coordinates) / sizeof(coordinates[0]); ++i) {
        check_motion(CMD_MOVE_DIRECT, coordinates[i][0], coordinates[i][1], 0, 0, 123);
        check_motion(CMD_MOVE_HUMAN, coordinates[i][0], coordinates[i][1], 0, -100, 123);
        check_motion(CMD_MOVE_HUMAN, coordinates[i][0], coordinates[i][1], 100, 100, 123);
    }
    uint32_t state = 0xACED1234u;
    for (unsigned i = 0; i < 400; ++i) {
        int16_t dx = (int16_t)((int32_t)(next_random(&state) & 0xFFFFu) - 32768);
        int16_t dy = (int16_t)((int32_t)(next_random(&state) & 0xFFFFu) - 32768);
        float speed = (float)(next_random(&state) % 101u);
        float curve = (float)((int32_t)(next_random(&state) % 201u) - 100);
        check_motion((i & 1u) ? CMD_MOVE_DIRECT : CMD_MOVE_HUMAN, dx, dy, speed, curve, state);
    }
}

static void test_mouse_backpressure(void) {
    reset_fixture(123);
    HidCommand command = {0};
    command.type = CMD_MOVE_HUMAN;
    command.dx = -32768;
    command.dy = 32767;
    command.speed = 100;
    command.curvature = -100;
    fixture.accept_mouse = false;
    CHECK(hid_engine_submit(&fixture.engine, &command));
    tick_after(0);
    for (unsigned i = 0; i < 100; ++i) tick_after(1000);
    CHECK(fixture.mouse_attempts > 0 && fixture.report_count == 0);
    CHECK(fixture.engine.sent_x == 0 && fixture.engine.sent_y == 0);
    CHECK(hid_engine_busy(&fixture.engine));
    fixture.accept_mouse = true;
    tick_after(1000);
    CHECK(fixture.report_count > 0);
    int32_t sent_x = fixture.engine.sent_x, sent_y = fixture.engine.sent_y;
    fixture.accept_mouse = false;
    uint32_t attempts = fixture.mouse_attempts;
    for (unsigned i = 0; i < 100 && fixture.mouse_attempts == attempts; ++i) tick_after(1000);
    CHECK(fixture.mouse_attempts > attempts);
    uint16_t step = fixture.engine.step;
    for (unsigned i = 0; i < 100; ++i) tick_after(1000);
    CHECK(fixture.engine.sent_x == sent_x && fixture.engine.sent_y == sent_y);
    CHECK(fixture.engine.step == step);
    fixture.accept_mouse = true;
    drain();
    CHECK(fixture.total_x == command.dx && fixture.total_y == command.dy);
}

static void test_keys_and_clicks(void) {
    HidCommand command = {0};
    command.type = CMD_PRESS_KEY;
    command.code = 4;
    command.hold_ms = 30;
    reset_fixture(123);
    CHECK(hid_engine_submit(&fixture.engine, &command));
    fixture.accept_keyboard = false;
    for (unsigned i = 0; i < 50; ++i) tick_after(1000);
    CHECK(fixture.report_count == 0 && fixture.keyboard_attempts > 0);
    fixture.accept_keyboard = true;
    tick_after(1000);
    CHECK(fixture.report_count == 1);
    CHECK(fixture.reports[0].kind == 2 && fixture.reports[0].code == 4);
    uint64_t press_at = fixture.reports[0].at_us;
    fixture.accept_keyboard = false;
    for (unsigned i = 0; i < 100; ++i) tick_after(1000);
    CHECK(fixture.report_count == 1 && hid_engine_busy(&fixture.engine));
    fixture.accept_keyboard = true;
    drain();
    CHECK(fixture.report_count == 2 && fixture.reports[1].code == 0);
    CHECK(fixture.reports[1].at_us >= press_at + 25000);

    for (uint8_t key = 0xE0; key <= 0xE7; ++key) {
        reset_fixture(123);
        command.code = key;
        command.hold_ms = 0;
        CHECK(hid_engine_submit(&fixture.engine, &command));
        drain();
        CHECK(fixture.report_count == 2);
        CHECK(fixture.reports[0].kind == 2 && fixture.reports[0].code == 0);
        CHECK(fixture.reports[0].modifier == (uint8_t)(1u << (key - 0xE0)));
        CHECK(fixture.reports[1].modifier == 0 && fixture.reports[1].code == 0);
    }
    reset_fixture(123);
    command.code = 0x28;
    command.hold_ms = 5000;
    CHECK(hid_engine_submit(&fixture.engine, &command));
    drain();
    CHECK(fixture.report_count == 2);
    uint64_t hold = fixture.reports[1].at_us - fixture.reports[0].at_us;
    CHECK(hold >= 4995000 && hold <= 5006000);

    reset_fixture(123);
    command.type = CMD_CLICK_BUTTON;
    command.code = 31;
    command.hold_ms = 0;
    CHECK(hid_engine_submit(&fixture.engine, &command));
    tick_after(0);
    CHECK(fixture.report_count == 1 && fixture.reports[0].code == 31);
    fixture.accept_mouse = false;
    for (unsigned i = 0; i < 150; ++i) tick_after(1000);
    CHECK(fixture.report_count == 1 && hid_engine_busy(&fixture.engine));
    fixture.accept_mouse = true;
    drain();
    CHECK(fixture.report_count == 2 && fixture.reports[1].code == 0);
    CHECK(fixture.total_x == 0 && fixture.total_y == 0);

    for (uint32_t seed = 1; seed <= 20; ++seed) {
        reset_fixture(seed);
        CHECK(hid_engine_submit(&fixture.engine, &command));
        drain();
        CHECK(fixture.report_count == 2);
        hold = fixture.reports[1].at_us - fixture.reports[0].at_us;
        CHECK(hold >= 60000 && hold <= 101000);
    }
}

static void test_queue(void) {
    reset_fixture(123);
    HidCommand command = {0};
    command.type = CMD_PRESS_KEY;
    command.code = 4;
    command.hold_ms = 100;
    for (unsigned i = 0; i < HID_QUEUE_CAPACITY; ++i) {
        command.code = (uint8_t)(4u + i);
        CHECK(hid_engine_submit(&fixture.engine, &command));
    }
    CHECK(!hid_engine_submit(&fixture.engine, &command));
    CHECK(fixture.engine.count == HID_QUEUE_CAPACITY);
    tick_after(0);
    CHECK(fixture.engine.active && fixture.engine.count == HID_QUEUE_CAPACITY - 1);
    command.code = 4 + HID_QUEUE_CAPACITY;
    CHECK(hid_engine_submit(&fixture.engine, &command));
    CHECK(fixture.engine.count == HID_QUEUE_CAPACITY);
    CHECK(!hid_engine_submit(&fixture.engine, &command));
    drain();
    CHECK(fixture.report_count == 2 * (HID_QUEUE_CAPACITY + 1));
    for (size_t i = 0; i < fixture.report_count; ++i) {
        CHECK(fixture.reports[i].kind == 2);
        CHECK(fixture.reports[i].code == ((i & 1u) ? 0 : (uint8_t)(4u + i / 2u)));
    }
}

static void check_neutral_reports(size_t from) {
    bool mouse = false, keyboard = false;
    for (size_t i = from; i < fixture.report_count; ++i) {
        Report *report = &fixture.reports[i];
        CHECK(report->code == 0 && report->modifier == 0 && report->dx == 0 && report->dy == 0);
        mouse |= report->kind == 1;
        keyboard |= report->kind == 2;
    }
    CHECK(mouse && keyboard);
}

static void test_disconnect_and_abort(void) {
    reset_fixture(123);
    HidCommand command = {0};
    command.type = CMD_PRESS_KEY;
    command.code = 4;
    command.hold_ms = 1000;
    CHECK(hid_engine_submit(&fixture.engine, &command));
    tick_after(0);
    CHECK(fixture.report_count == 1 && fixture.reports[0].code == 4);
    command.type = CMD_MOVE_DIRECT;
    command.dx = 500;
    CHECK(hid_engine_submit(&fixture.engine, &command));
    size_t before = fixture.report_count;
    tick_at(fixture.now_us + 1000, false);
    CHECK(!fixture.engine.active && fixture.engine.count == 0);
    CHECK(fixture.engine.release_mouse && fixture.engine.release_keyboard);
    CHECK(hid_engine_busy(&fixture.engine) && fixture.report_count == before);
    tick_at(fixture.now_us + 1000, false);
    CHECK(fixture.report_count == before);
    fixture.accept_mouse = false;
    fixture.accept_keyboard = false;
    tick_after(1000);
    CHECK(hid_engine_busy(&fixture.engine) && fixture.report_count == before);
    fixture.accept_mouse = true;
    fixture.accept_keyboard = true;
    drain();
    check_neutral_reports(before);
    CHECK(fixture.total_x == 0 && fixture.total_y == 0);
    CHECK(hid_engine_submit(&fixture.engine, &command));
    drain();
    CHECK(fixture.total_x == 500);

    reset_fixture(123);
    command.type = CMD_CLICK_BUTTON;
    command.code = 1;
    CHECK(hid_engine_submit(&fixture.engine, &command));
    tick_after(0);
    before = fixture.report_count;
    hid_engine_abort(&fixture.engine);
    CHECK(hid_engine_busy(&fixture.engine));
    drain();
    check_neutral_reports(before);
}

static void test_usb_stall(void) {
    reset_fixture(123);
    HidCommand command = {0};
    command.type = CMD_MOVE_DIRECT;
    command.dx = 1000;
    CHECK(hid_engine_submit(&fixture.engine, &command));
    fixture.accept_mouse = false;
    fixture.accept_keyboard = false;
    tick_after(0);
    uint64_t started = fixture.now_us;
    tick_at(started + HID_USB_STALL_US - 1u, true);
    CHECK(fixture.engine.active);
    tick_at(started + HID_USB_STALL_US + 1u, true);
    CHECK(!fixture.engine.active && fixture.engine.count == 0);
    CHECK(fixture.engine.release_mouse && fixture.engine.release_keyboard);
    CHECK(fixture.report_count == 0 && hid_engine_busy(&fixture.engine));
    fixture.accept_mouse = true;
    fixture.accept_keyboard = true;
    drain();
    check_neutral_reports(0);
    CHECK(fixture.total_x == 0);
}

static void test_independent_release_retries(void) {
    reset_fixture(123);
    hid_engine_abort(&fixture.engine);
    fixture.accept_keyboard = false;
    tick_after(0);
    CHECK(fixture.keyboard_attempts == 1 && fixture.mouse_attempts == 1);
    CHECK(fixture.engine.release_keyboard && !fixture.engine.release_mouse);
    CHECK(fixture.report_count == 1 && fixture.reports[0].kind == 1);
    CHECK(fixture.reports[0].code == 0);
    CHECK(hid_engine_busy(&fixture.engine));
    tick_after(1000);
    CHECK(fixture.keyboard_attempts == 2 && fixture.mouse_attempts == 1);
    fixture.accept_keyboard = true;
    drain();
    CHECK(fixture.report_count == 2 && fixture.reports[1].kind == 2);
    CHECK(fixture.reports[1].code == 0 && fixture.reports[1].modifier == 0);

    reset_fixture(123);
    hid_engine_abort(&fixture.engine);
    fixture.accept_mouse = false;
    tick_after(0);
    CHECK(fixture.keyboard_attempts == 1 && fixture.mouse_attempts == 1);
    CHECK(!fixture.engine.release_keyboard && fixture.engine.release_mouse);
    CHECK(fixture.report_count == 1 && fixture.reports[0].kind == 2);
    CHECK(hid_engine_busy(&fixture.engine));
    fixture.accept_mouse = true;
    drain();
    CHECK(fixture.report_count == 2 && fixture.reports[1].kind == 1);
    CHECK(fixture.reports[1].code == 0);
}

static void test_health(void) {
    uint8_t packet[HID_HEALTH_SIZE];
    memset(packet, 0xFF, sizeof(packet));
    hid_health_encode(packet, 0x78563412u, 0xA5, 7, 0x12345678u, 0xFEDCBA98u, 0x98765432u);
    CHECK(memcmp(packet, "PICO", 4) == 0);
    CHECK(packet[4] == 2 && packet[5] == 0xA5 && packet[6] == 7 && packet[7] == HID_QUEUE_CAPACITY);
    CHECK(packet[8] == 0x12 && packet[9] == 0x34 && packet[10] == 0x56 && packet[11] == 0x78);
    CHECK(hid_read_u32(packet + 8) == 0x78563412u);
    CHECK(hid_read_u32(packet + 12) == 0x12345678u);
    CHECK(hid_read_u32(packet + 16) == 0xFEDCBA98u);
    CHECK(hid_read_u32(packet + 20) == 0x98765432u);
}

static void run_test(const char *name, void (*test)(void)) {
    test();
    printf("PASS %s\n", name);
}

int main(void) {
    run_test("wire packet validation", test_decode);
    run_test("boundary and randomized motion precision", test_motion);
    run_test("USB movement backpressure", test_mouse_backpressure);
    run_test("keyboard modifiers, timing, clicks and release retries", test_keys_and_clicks);
    run_test("queue capacity and FIFO execution", test_queue);
    run_test("disconnect, abort and recovery releases", test_disconnect_and_abort);
    run_test("USB stall timeout", test_usb_stall);
    run_test("independent mouse and keyboard release retries", test_independent_release_retries);
    run_test("24-byte health encoding", test_health);
    puts("All HID engine tests passed.");
    return 0;
}
