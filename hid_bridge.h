#ifndef HID_BRIDGE_H
#define HID_BRIDGE_H

#include "hid_engine.h"
#include "hid_lease.h"
#include "hid_diagnostics.h"
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
    HidLease lease;
    HidDiagnostics diagnostics;
    uint32_t clock_ms;
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

/* Set once before launching Core 1. Production uses hardware randomness. */
void hid_bridge_set_boot(HidBridge *bridge, uint32_t boot);
/* Core 1, under the lwIP lock; never waits for USB or executes legacy input. */
void hid_bridge_lease_request(HidBridge *bridge, const uint8_t *data, size_t size,
                              uint32_t now_ms, uint8_t response[HID_LEASE_RESPONSE_SIZE]);
/* Core 0. Callback must be a nonblocking raw keyboard report submission and
 * must not reenter the bridge. Its execution is linearized with cancellation. */
void hid_bridge_lease_tick(HidBridge *bridge, uint32_t now_ms,
                          bool (*keyboard)(void *, uint8_t, const uint8_t[6]), void *context);

/* Timed production variants reuse the existing bridge lock/loop timestamp. */
void hid_bridge_network_at(HidBridge *, bool up, bool force_abort, uint32_t now_ms);
void hid_bridge_cancel_reason(HidBridge *, uint32_t reason, uint32_t now_ms);
void hid_bridge_publish_at(HidBridge *, uint32_t epoch, uint8_t flags, bool usb_available,
                           bool busy, uint32_t rejected_delta, uint64_t now_us);
void hid_bridge_network_heartbeat_at(HidBridge *, uint64_t now_us);
void hid_bridge_diagnostic_request(HidBridge *, const uint8_t *, size_t, uint32_t now_ms,
                                   uint8_t response[HID_DIAGNOSTIC_SIZE]);
void hid_bridge_note_send_error(HidBridge *);
void hid_bridge_release_at(HidBridge *, uint32_t now_ms);
/* Requires bridge lock already held. Old engine uses this only after TinyUSB
 * accepts a report, so its reports cannot be confused with an older lease. */
void hid_bridge_keyboard_submitted_locked(HidBridge *, uint8_t modifier,
                                         const uint8_t keys[6], uint32_t now_ms,
                                         bool lease_state);
/* Core 0 from the real TinyUSB completion callback, never from SET acceptance. */
void hid_bridge_keyboard_complete(HidBridge *, uint8_t modifier, const uint8_t keys[6],
                                  uint32_t now_ms, bool valid_report);

#endif
