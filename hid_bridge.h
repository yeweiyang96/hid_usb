#ifndef HID_BRIDGE_H
#define HID_BRIDGE_H

#include "hid_engine.h"
#include "pico/critical_section.h"

/* All fields are private to hid_bridge.c after initialization. One short IRQ-
 * and multicore-safe lock protects admission, cancellation and status together.
 * No caller may acquire the lwIP lock while holding this lock. */
typedef struct {
    critical_section_t lock;
    HidCommand commands[HID_QUEUE_CAPACITY];
    uint8_t head, count, usb_flags;
    bool network_up, usb_available, in_flight, engine_busy;
    uint32_t epoch, acknowledged_epoch, accepted, rejected;
    uint32_t network_heartbeat_ms;
} HidBridge;

typedef struct {
    uint8_t flags, queued;
    bool available;
    uint32_t accepted, rejected, epoch, acknowledged_epoch;
    uint32_t network_heartbeat_ms;
} HidBridgeStatus;

void hid_bridge_init(HidBridge *bridge);
/* Core 1 / its network IRQ. Rejoining does not open admission until Core 0
 * acknowledges the cancellation epoch. force_abort invalidates even if down. */
void hid_bridge_network(HidBridge *bridge, bool up, bool force_abort);
/* Core 0 transfer failure/stall: drop queued work without changing Wi-Fi state. */
void hid_bridge_cancel(HidBridge *bridge);
bool hid_bridge_submit(HidBridge *bridge, const HidCommand *command);
void hid_bridge_reject(HidBridge *bridge);
void hid_bridge_network_heartbeat(HidBridge *bridge, uint32_t now_ms);
HidBridgeStatus hid_bridge_status(HidBridge *bridge);
uint32_t hid_bridge_epoch(HidBridge *bridge);

/* Core 0 only. Take at most one command, only when its private engine is idle.
 * Take immediately marks in-flight, so health cannot claim false completion. */
bool hid_bridge_take(HidBridge *bridge, uint32_t epoch, HidCommand *command);
void hid_bridge_publish(HidBridge *bridge, uint32_t epoch, uint8_t usb_flags,
                         bool usb_available, bool busy, uint32_t rejected_delta);
/* A short output lease linearizes one nonblocking USB report with cancellation.
 * Release reports (all-zero HID state) are permitted while network is down.
 * A successful begin MUST be paired with end; a failed begin releases itself. */
bool hid_bridge_output_begin(HidBridge *bridge, uint32_t epoch, bool release);
void hid_bridge_output_end(HidBridge *bridge);

#endif
