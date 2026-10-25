#include "hid_bridge.h"
#include <string.h>

static bool available(const HidBridge *b) {
    return b->network_up && b->usb_available && b->acknowledged_epoch == b->epoch;
}

static void invalidate(HidBridge *b) {
    ++b->epoch;
    b->rejected += b->count;
    b->count = b->head = 0;
    /* In-flight/engine state is cleared only by the owning core's publish. */
}

void hid_bridge_init(HidBridge *b) {
    memset(b, 0, sizeof(*b));
    critical_section_init(&b->lock);
    b->epoch = 1;
}

void hid_bridge_network(HidBridge *b, bool up, bool force_abort) {
    critical_section_enter_blocking(&b->lock);
    if ((!up && b->network_up) || force_abort) invalidate(b);
    b->network_up = up;
    critical_section_exit(&b->lock);
}

bool hid_bridge_submit(HidBridge *b, const HidCommand *command) {
    critical_section_enter_blocking(&b->lock);
    bool ok = available(b) && b->count < HID_QUEUE_CAPACITY;
    if (ok) {
        b->commands[(b->head + b->count) % HID_QUEUE_CAPACITY] = *command;
        ++b->count;
        ++b->accepted;
    } else {
        ++b->rejected;
    }
    critical_section_exit(&b->lock);
    return ok;
}

void hid_bridge_cancel(HidBridge *b) {
    critical_section_enter_blocking(&b->lock);
    invalidate(b);
    critical_section_exit(&b->lock);
}

void hid_bridge_reject(HidBridge *b) {
    critical_section_enter_blocking(&b->lock);
    ++b->rejected;
    critical_section_exit(&b->lock);
}

void hid_bridge_network_heartbeat(HidBridge *b, uint32_t now_ms) {
    critical_section_enter_blocking(&b->lock);
    b->network_heartbeat_ms = now_ms;
    critical_section_exit(&b->lock);
}

uint32_t hid_bridge_epoch(HidBridge *b) {
    critical_section_enter_blocking(&b->lock);
    uint32_t epoch = b->epoch;
    critical_section_exit(&b->lock);
    return epoch;
}

HidBridgeStatus hid_bridge_status(HidBridge *b) {
    critical_section_enter_blocking(&b->lock);
    bool busy = b->count || b->in_flight || b->engine_busy || b->acknowledged_epoch != b->epoch;
    HidBridgeStatus result = {
        .flags = b->usb_flags | (busy ? 8u : 0u) | (b->network_up ? 16u : 0u),
        .queued = b->count,
        .available = available(b),
        .accepted = b->accepted,
        .rejected = b->rejected,
        .epoch = b->epoch,
        .acknowledged_epoch = b->acknowledged_epoch,
        .network_heartbeat_ms = b->network_heartbeat_ms
    };
    critical_section_exit(&b->lock);
    return result;
}

bool hid_bridge_take(HidBridge *b, uint32_t epoch, HidCommand *command) {
    critical_section_enter_blocking(&b->lock);
    bool ok = available(b) && epoch == b->epoch && b->count && !b->in_flight && !b->engine_busy;
    if (ok) {
        *command = b->commands[b->head];
        b->head = (uint8_t)((b->head + 1u) % HID_QUEUE_CAPACITY);
        --b->count;
        b->in_flight = true;
    }
    critical_section_exit(&b->lock);
    return ok;
}

void hid_bridge_publish(HidBridge *b, uint32_t epoch, uint8_t flags,
                         bool usb_available, bool busy, uint32_t rejected_delta) {
    critical_section_enter_blocking(&b->lock);
    b->rejected += rejected_delta;
    b->usb_flags = flags & 7u;
    if (!usb_available && b->usb_available) invalidate(b);
    b->usb_available = usb_available;
    if (epoch == b->epoch) {
        b->acknowledged_epoch = epoch;
        b->engine_busy = busy;
        b->in_flight = busy;
    }
    critical_section_exit(&b->lock);
}

bool hid_bridge_output_begin(HidBridge *b, uint32_t epoch, bool release) {
    critical_section_enter_blocking(&b->lock);
    if (!release && (!available(b) || epoch != b->epoch)) {
        critical_section_exit(&b->lock);
        return false;
    }
    return true;
}

void hid_bridge_output_end(HidBridge *b) {
    critical_section_exit(&b->lock);
}
