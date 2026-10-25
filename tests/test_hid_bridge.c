#include "hid_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUSY_FLAG 8u
#define NETWORK_FLAG 16u
#define STRESS_COMMANDS 30000u

static void fail(const char *expression, int line) {
    fprintf(stderr, "FAIL bridge line %d: %s\n", line, expression);
    exit(1);
}

#define CHECK(expression) do { if (!(expression)) fail(#expression, __LINE__); } while (0)

static HidCommand command_number(unsigned number) {
    HidCommand command = {0};
    command.type = CMD_MOVE_DIRECT;
    command.dx = (int16_t)number;
    command.hold_ms = (uint16_t)number;
    return command;
}

static uint32_t ready(HidBridge *bridge) {
    hid_bridge_init(bridge);
    uint32_t epoch = hid_bridge_epoch(bridge);
    CHECK(epoch != 0);
    hid_bridge_publish(bridge, epoch, 7, true, false, 0);
    CHECK(!hid_bridge_status(bridge).available);
    hid_bridge_network(bridge, true, false);
    HidBridgeStatus status = hid_bridge_status(bridge);
    CHECK(status.available && status.flags == (7u | NETWORK_FLAG));
    CHECK(!status.queued && !status.accepted && !status.rejected);
    CHECK(status.epoch == epoch && status.acknowledged_epoch == epoch);
    return epoch;
}

static void test_capacity_fifo(void) {
    HidBridge bridge;
    uint32_t epoch = ready(&bridge);
    for (unsigned i = 1; i <= HID_QUEUE_CAPACITY; ++i) {
        HidCommand command = command_number(i);
        CHECK(hid_bridge_submit(&bridge, &command));
        HidBridgeStatus status = hid_bridge_status(&bridge);
        CHECK(status.accepted == i && status.queued == i);
        CHECK(status.flags & BUSY_FLAG);
    }
    HidCommand next = command_number(9);
    CHECK(!hid_bridge_submit(&bridge, &next));
    HidCommand taken;
    CHECK(hid_bridge_take(&bridge, epoch, &taken));
    CHECK(taken.hold_ms == 1);
    CHECK(!hid_bridge_take(&bridge, epoch, &taken));
    CHECK(hid_bridge_submit(&bridge, &next));
    CHECK(!hid_bridge_submit(&bridge, &next));
    HidBridgeStatus full = hid_bridge_status(&bridge);
    CHECK(full.queued == HID_QUEUE_CAPACITY && full.accepted == 9 && full.rejected == 2);
    hid_bridge_publish(&bridge, epoch, 7, true, true, 0);
    CHECK(!hid_bridge_take(&bridge, epoch, &taken));
    for (unsigned i = 2; i <= 9; ++i) {
        hid_bridge_publish(&bridge, epoch, 7, true, false, 0);
        CHECK(hid_bridge_take(&bridge, epoch, &taken));
        CHECK(taken.hold_ms == i);
    }
    CHECK(hid_bridge_status(&bridge).flags & BUSY_FLAG);
    hid_bridge_publish(&bridge, epoch, 7, true, false, 0);
    HidBridgeStatus done = hid_bridge_status(&bridge);
    CHECK(!done.queued && !(done.flags & BUSY_FLAG));
    CHECK(!hid_bridge_take(&bridge, epoch, &taken));
    puts("PASS bridge strict eight waiting plus one in-flight capacity, immediate busy and FIFO");
}

typedef struct {
    HidBridge *bridge;
    uint32_t epoch;
    HANDLE taken, finish;
} PausedConsumer;

static DWORD WINAPI take_then_pause(LPVOID argument) {
    PausedConsumer *context = (PausedConsumer *)argument;
    HidCommand command;
    CHECK(hid_bridge_take(context->bridge, context->epoch, &command));
    CHECK(command.hold_ms == 42);
    CHECK(SetEvent(context->taken));
    CHECK(WaitForSingleObject(context->finish, 5000) == WAIT_OBJECT_0);
    hid_bridge_publish(context->bridge, context->epoch, 7, true, false, 0);
    return 0;
}

static void join_thread(HANDLE thread) {
    CHECK(WaitForSingleObject(thread, 15000) == WAIT_OBJECT_0);
    DWORD exit_code = 1;
    CHECK(GetExitCodeThread(thread, &exit_code) && exit_code == 0);
    CHECK(CloseHandle(thread));
}

static void test_take_has_no_idle_gap(void) {
    HidBridge bridge;
    uint32_t epoch = ready(&bridge);
    HidCommand command = command_number(42);
    CHECK(hid_bridge_submit(&bridge, &command));
    PausedConsumer context = {&bridge, epoch, NULL, NULL};
    context.taken = CreateEventW(NULL, TRUE, FALSE, NULL);
    context.finish = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(context.taken && context.finish);
    HANDLE thread = CreateThread(NULL, 0, take_then_pause, &context, 0, NULL);
    CHECK(thread);
    CHECK(WaitForSingleObject(context.taken, 5000) == WAIT_OBJECT_0);
    for (unsigned i = 0; i < 10000; ++i) {
        HidBridgeStatus status = hid_bridge_status(&bridge);
        CHECK(status.accepted == 1 && !status.queued && (status.flags & BUSY_FLAG));
    }
    CHECK(SetEvent(context.finish));
    join_thread(thread);
    CHECK(!(hid_bridge_status(&bridge).flags & BUSY_FLAG));
    CHECK(CloseHandle(context.taken) && CloseHandle(context.finish));
    puts("PASS bridge take remains busy until Core 0 publishes completion");
}

static void test_epoch_abort_and_rejoin(void) {
    HidBridge bridge;
    uint32_t old_epoch = ready(&bridge);
    HidCommand command = command_number(1), taken;
    CHECK(hid_bridge_submit(&bridge, &command));
    CHECK(hid_bridge_take(&bridge, old_epoch, &taken));
    for (unsigned i = 0; i < HID_QUEUE_CAPACITY; ++i) CHECK(hid_bridge_submit(&bridge, &command));
    hid_bridge_network(&bridge, false, false);
    HidBridgeStatus down = hid_bridge_status(&bridge);
    CHECK(down.epoch == old_epoch + 1 && !down.available && !down.queued);
    CHECK(down.rejected == HID_QUEUE_CAPACITY && (down.flags & BUSY_FLAG));
    CHECK(!(down.flags & NETWORK_FLAG));
    hid_bridge_network(&bridge, false, false);
    CHECK(hid_bridge_epoch(&bridge) == down.epoch);
    hid_bridge_network(&bridge, true, false);
    CHECK(!hid_bridge_submit(&bridge, &command));
    CHECK(!hid_bridge_take(&bridge, old_epoch, &taken));
    /* The old engine reports its active-command cancellation once. Its stale
     * publish cannot acknowledge the new epoch or reopen command admission. */
    hid_bridge_publish(&bridge, old_epoch, 7, true, false, 1);
    HidBridgeStatus waiting = hid_bridge_status(&bridge);
    CHECK(waiting.acknowledged_epoch == old_epoch && !waiting.available);
    CHECK(waiting.rejected == HID_QUEUE_CAPACITY + 2u && (waiting.flags & BUSY_FLAG));
    CHECK(!hid_bridge_submit(&bridge, &command));
    CHECK(!hid_bridge_output_begin(&bridge, old_epoch, false));
    CHECK(!hid_bridge_output_begin(&bridge, down.epoch, false));
    CHECK(hid_bridge_output_begin(&bridge, old_epoch, true));
    hid_bridge_output_end(&bridge);
    hid_bridge_publish(&bridge, down.epoch, 7, true, false, 0);
    HidBridgeStatus acknowledged = hid_bridge_status(&bridge);
    CHECK(acknowledged.available && !(acknowledged.flags & BUSY_FLAG));
    CHECK(!hid_bridge_take(&bridge, down.epoch, &taken));
    CHECK(!hid_bridge_output_begin(&bridge, old_epoch, false));
    CHECK(hid_bridge_output_begin(&bridge, down.epoch, false));
    hid_bridge_output_end(&bridge);
    CHECK(hid_bridge_submit(&bridge, &command));
    CHECK(hid_bridge_take(&bridge, down.epoch, &taken));
    CHECK(taken.hold_ms == 1);
    /* A reset of the Wi-Fi driver invalidates even an already-down link. */
    hid_bridge_network(&bridge, false, true);
    CHECK(hid_bridge_epoch(&bridge) == down.epoch + 1);
    CHECK(hid_bridge_output_begin(&bridge, old_epoch, true));
    hid_bridge_output_end(&bridge);
    puts("PASS bridge full-queue cancellation, rejoin acknowledgment and stale-epoch output gate");
}

static void test_usb_loss(void) {
    HidBridge bridge;
    uint32_t epoch = ready(&bridge);
    HidCommand command = command_number(3), taken;
    CHECK(hid_bridge_submit(&bridge, &command));
    CHECK(hid_bridge_take(&bridge, epoch, &taken));
    CHECK(hid_bridge_submit(&bridge, &command));
    CHECK(hid_bridge_submit(&bridge, &command));
    hid_bridge_publish(&bridge, epoch, 0xff, false, true, 0);
    HidBridgeStatus lost = hid_bridge_status(&bridge);
    CHECK(!lost.available && !lost.queued && lost.rejected == 2);
    CHECK(lost.epoch == epoch + 1 && lost.acknowledged_epoch == epoch);
    CHECK((lost.flags & 7u) == 7u);
    CHECK(!hid_bridge_submit(&bridge, &command));
    CHECK(!hid_bridge_output_begin(&bridge, epoch, false));
    CHECK(hid_bridge_output_begin(&bridge, epoch, true));
    hid_bridge_output_end(&bridge);
    hid_bridge_publish(&bridge, lost.epoch, 0, false, false, 1);
    CHECK(!hid_bridge_status(&bridge).available);
    CHECK(!hid_bridge_take(&bridge, lost.epoch, &taken));
    hid_bridge_publish(&bridge, lost.epoch, 7, true, false, 0);
    CHECK(hid_bridge_status(&bridge).available);
    CHECK(hid_bridge_submit(&bridge, &command));
    CHECK(hid_bridge_take(&bridge, lost.epoch, &taken));
    puts("PASS bridge USB loss clears waiting work and requires the current epoch");
}

static void test_core0_cancel(void) {
    HidBridge bridge;
    uint32_t epoch = ready(&bridge);
    HidCommand command = command_number(5), taken;
    CHECK(hid_bridge_submit(&bridge, &command));
    CHECK(hid_bridge_take(&bridge, epoch, &taken));
    for (unsigned i = 0; i < HID_QUEUE_CAPACITY; ++i) CHECK(hid_bridge_submit(&bridge, &command));
    hid_bridge_cancel(&bridge);
    HidBridgeStatus canceled = hid_bridge_status(&bridge);
    CHECK(canceled.epoch == epoch + 1 && canceled.acknowledged_epoch == epoch);
    CHECK(canceled.rejected == HID_QUEUE_CAPACITY && !canceled.queued);
    CHECK((canceled.flags & (NETWORK_FLAG | BUSY_FLAG)) == (NETWORK_FLAG | BUSY_FLAG));
    CHECK(!canceled.available && !hid_bridge_submit(&bridge, &command));
    CHECK(!hid_bridge_output_begin(&bridge, epoch, false));
    hid_bridge_publish(&bridge, epoch, 7, true, true, 1);
    CHECK(!hid_bridge_status(&bridge).available);
    /* Core 0 has acknowledged cancellation but is still retrying zero releases.
     * New work may wait, but cannot be taken until those releases finish. */
    hid_bridge_publish(&bridge, canceled.epoch, 7, true, true, 0);
    CHECK(hid_bridge_status(&bridge).available);
    CHECK(hid_bridge_submit(&bridge, &command));
    CHECK(!hid_bridge_take(&bridge, canceled.epoch, &taken));
    hid_bridge_publish(&bridge, canceled.epoch, 7, true, false, 0);
    CHECK(hid_bridge_take(&bridge, canceled.epoch, &taken));
    CHECK(taken.hold_ms == 5);
    CHECK(hid_bridge_status(&bridge).rejected == HID_QUEUE_CAPACITY + 2u);
    puts("PASS bridge Core 0 cancel clears a full queue while preserving Wi-Fi and release ordering");
}

static void test_counter_wrap_and_heartbeat(void) {
    HidBridge bridge;
    uint32_t epoch = ready(&bridge);
    hid_bridge_publish(&bridge, epoch, 7, true, false, UINT32_MAX - 1u);
    HidEngine engine;
    HidTransport transport = {0};
    hid_engine_init(&engine, transport, 1);
    /* Counter seeding avoids billions of operations; the wrap itself is
     * generated by the unmodified engine's aborting of three real commands. */
    engine.rejected = UINT32_MAX - 1u;
    uint32_t previous_engine_rejected = engine.rejected;
    HidCommand command = command_number(1);
    for (unsigned i = 0; i < 3; ++i) CHECK(hid_engine_submit(&engine, &command));
    hid_engine_abort(&engine);
    uint32_t delta = engine.rejected - previous_engine_rejected;
    CHECK(delta == 3);
    hid_bridge_publish(&bridge, epoch, 7, true, false, delta);
    CHECK(hid_bridge_status(&bridge).rejected == 1);
    hid_bridge_reject(&bridge);
    CHECK(hid_bridge_status(&bridge).rejected == 2);
    hid_bridge_network_heartbeat(&bridge, UINT32_MAX);
    CHECK(hid_bridge_status(&bridge).network_heartbeat_ms == UINT32_MAX);
    hid_bridge_network_heartbeat(&bridge, 5);
    CHECK(hid_bridge_status(&bridge).network_heartbeat_ms == 5);
    puts("PASS bridge uint32 rejection-delta wrap and heartbeat publication");
}

typedef struct {
    HidBridge bridge;
    uint32_t epoch;
    unsigned mouse_pressed, mouse_released, keyboard_released;
} ReleaseFixture;

static bool release_mouse(void *argument, uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel) {
    ReleaseFixture *fixture = (ReleaseFixture *)argument;
    bool zero = !buttons && !dx && !dy && !wheel;
    CHECK(hid_bridge_output_begin(&fixture->bridge, fixture->epoch, zero));
    if (zero) ++fixture->mouse_released;
    else ++fixture->mouse_pressed;
    hid_bridge_output_end(&fixture->bridge);
    return true;
}

static bool release_keyboard(void *argument, uint8_t modifier, const uint8_t keys[6]) {
    ReleaseFixture *fixture = (ReleaseFixture *)argument;
    CHECK(modifier == 0);
    for (unsigned i = 0; i < 6; ++i) CHECK(keys[i] == 0);
    CHECK(hid_bridge_output_begin(&fixture->bridge, fixture->epoch, true));
    ++fixture->keyboard_released;
    hid_bridge_output_end(&fixture->bridge);
    return true;
}

static void test_engine_zero_release_after_disconnect(void) {
    ReleaseFixture fixture;
    memset(&fixture, 0, sizeof(fixture));
    fixture.epoch = ready(&fixture.bridge);
    HidTransport transport = {release_mouse, release_keyboard, &fixture};
    HidEngine engine;
    hid_engine_init(&engine, transport, 1);
    HidCommand command = {0};
    command.type = CMD_TOGGLE_BUTTON;
    command.code = 1;
    command.down = true;
    CHECK(hid_engine_submit(&engine, &command));
    hid_engine_tick(&engine, 1000, true);
    CHECK(fixture.mouse_pressed == 1 && engine.held_buttons == 1);
    hid_bridge_network(&fixture.bridge, false, false);
    CHECK(!hid_bridge_output_begin(&fixture.bridge, fixture.epoch, false));
    hid_engine_abort(&engine);
    /* USB is still available even though the network is down; this lets the
     * engine retry releases through the stale-epoch release-only lease. */
    hid_engine_tick(&engine, 2000, true);
    CHECK(fixture.mouse_released == 1 && fixture.keyboard_released == 1);
    CHECK(!hid_engine_busy(&engine) && !engine.held_buttons);
    puts("PASS production engine sends all-zero mouse and keyboard releases through a stale/down lease");
}

typedef struct {
    HidBridge bridge;
    uint32_t epoch;
    volatile LONG producer_done, consumer_done;
    volatile LONG snapshots;
} Stress;

static DWORD WINAPI stress_producer(LPVOID argument) {
    Stress *stress = (Stress *)argument;
    for (unsigned i = 1; i <= STRESS_COMMANDS; ++i) {
        HidCommand command = command_number(i);
        while (!hid_bridge_submit(&stress->bridge, &command)) SwitchToThread();
        if ((i & 63u) == 0) hid_bridge_network_heartbeat(&stress->bridge, i);
    }
    InterlockedExchange(&stress->producer_done, 1);
    return 0;
}

static DWORD WINAPI stress_consumer(LPVOID argument) {
    Stress *stress = (Stress *)argument;
    for (unsigned i = 1; i <= STRESS_COMMANDS; ++i) {
        HidCommand command;
        while (!hid_bridge_take(&stress->bridge, stress->epoch, &command)) SwitchToThread();
        CHECK(command.hold_ms == i && command.dx == (int16_t)i);
        CHECK(hid_bridge_output_begin(&stress->bridge, stress->epoch, false));
        hid_bridge_output_end(&stress->bridge);
        /* Exercise both publications while the concurrent producer and health
         * reader contend for the same production bridge lock. */
        hid_bridge_publish(&stress->bridge, stress->epoch, 7, true, true, 0);
        hid_bridge_publish(&stress->bridge, stress->epoch, 7, true, false, 0);
    }
    InterlockedExchange(&stress->consumer_done, 1);
    return 0;
}

static DWORD WINAPI stress_snapshots(LPVOID argument) {
    Stress *stress = (Stress *)argument;
    uint32_t accepted = 0, rejected = 0, heartbeat = 0;
    do {
        HidBridgeStatus status = hid_bridge_status(&stress->bridge);
        CHECK(status.available && status.epoch == stress->epoch);
        CHECK(status.acknowledged_epoch == stress->epoch);
        CHECK(status.queued <= HID_QUEUE_CAPACITY);
        CHECK(!status.queued || (status.flags & BUSY_FLAG));
        CHECK(status.accepted >= accepted && status.accepted <= STRESS_COMMANDS);
        CHECK(status.rejected >= rejected);
        CHECK(status.network_heartbeat_ms >= heartbeat);
        CHECK((status.flags & (7u | NETWORK_FLAG)) == (7u | NETWORK_FLAG));
        accepted = status.accepted;
        rejected = status.rejected;
        heartbeat = status.network_heartbeat_ms;
        InterlockedIncrement(&stress->snapshots);
    } while (!InterlockedCompareExchange(&stress->producer_done, 0, 0) ||
             !InterlockedCompareExchange(&stress->consumer_done, 0, 0));
    return 0;
}

static void test_concurrent_fifo_and_status(void) {
    Stress stress;
    memset(&stress, 0, sizeof(stress));
    stress.epoch = ready(&stress.bridge);
    HANDLE reader = CreateThread(NULL, 0, stress_snapshots, &stress, 0, NULL);
    HANDLE consumer = CreateThread(NULL, 0, stress_consumer, &stress, 0, NULL);
    HANDLE producer = CreateThread(NULL, 0, stress_producer, &stress, 0, NULL);
    CHECK(reader && consumer && producer);
    join_thread(producer);
    join_thread(consumer);
    join_thread(reader);
    HidBridgeStatus status = hid_bridge_status(&stress.bridge);
    CHECK(status.accepted == STRESS_COMMANDS && !status.queued && !(status.flags & BUSY_FLAG));
    CHECK(InterlockedCompareExchange(&stress.snapshots, 0, 0) > 0);
    puts("PASS bridge 30000 FIFO commands with concurrent producer, consumer and status reader");
}

typedef struct {
    HidBridge *bridge;
    HANDLE started, finished;
} CancelThread;

static DWORD WINAPI cancel_during_output(LPVOID argument) {
    CancelThread *context = (CancelThread *)argument;
    CHECK(SetEvent(context->started));
    hid_bridge_network(context->bridge, false, false);
    CHECK(SetEvent(context->finished));
    return 0;
}

static void test_output_lease_serializes_disconnect(void) {
    HidBridge bridge;
    uint32_t epoch = ready(&bridge);
    CancelThread context = {&bridge, NULL, NULL};
    context.started = CreateEventW(NULL, TRUE, FALSE, NULL);
    context.finished = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(context.started && context.finished);
    CHECK(hid_bridge_output_begin(&bridge, epoch, false));
    HANDLE thread = CreateThread(NULL, 0, cancel_during_output, &context, 0, NULL);
    CHECK(thread);
    CHECK(WaitForSingleObject(context.started, 5000) == WAIT_OBJECT_0);
    /* Cancellation must not finish while a report owns the output lease. */
    CHECK(WaitForSingleObject(context.finished, 25) == WAIT_TIMEOUT);
    hid_bridge_output_end(&bridge);
    CHECK(WaitForSingleObject(context.finished, 5000) == WAIT_OBJECT_0);
    join_thread(thread);
    CHECK(hid_bridge_epoch(&bridge) == epoch + 1);
    CHECK(!hid_bridge_output_begin(&bridge, epoch, false));
    CHECK(hid_bridge_output_begin(&bridge, epoch, true));
    hid_bridge_output_end(&bridge);
    CHECK(CloseHandle(context.started) && CloseHandle(context.finished));
    puts("PASS bridge output lease and disconnect are mutually exclusive; zero release remains permitted");
}

int main(void) {
    test_capacity_fifo();
    test_take_has_no_idle_gap();
    test_epoch_abort_and_rejoin();
    test_usb_loss();
    test_core0_cancel();
    test_counter_wrap_and_heartbeat();
    test_engine_zero_release_after_disconnect();
    test_concurrent_fifo_and_status();
    test_output_lease_serializes_disconnect();
    puts("All nine HID bridge groups passed (host lock model; hardware IRQ timing is not tested).");
    return 0;
}
