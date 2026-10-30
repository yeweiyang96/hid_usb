#ifndef HID_DIAGNOSTICS_H
#define HID_DIAGNOSTICS_H
#include <stdbool.h>
#include <stdint.h>

#define HID_DIAGNOSTIC_SIZE 160u
#define HID_FIRMWARE_VERSION 0x00060000u /* major.minor.patch.build = 0.6.0.0 */
#define HID_CAPABILITIES 0x7fu /* v3 capabilities plus bounded v4 timelines */
enum { HID_STOP_NONE=0, HID_STOP_CLIENT=1, HID_STOP_EXPIRED=2, HID_STOP_WIFI=3,
       HID_STOP_USB_UNAVAILABLE=4, HID_STOP_USB_FAILED=5, HID_STOP_USB_STALL=6,
       HID_STOP_LEGACY_RELEASE=7, HID_STOP_INTERNAL=8, HID_STOP_WINDOW_END=9,
       HID_STOP_TIMELINE_LATE=10 };
enum { HID_VALID_STOP=1, HID_VALID_SUBMITTED=2, HID_VALID_COMPLETED=4,
       HID_VALID_RELEASE_SUBMITTED=8, HID_VALID_RELEASE_COMPLETED=16,
       HID_SUBMITTED_RELEASE=256, HID_COMPLETED_RELEASE=512 };
typedef struct { uint32_t epoch, session, seq, time_ms; } HidDiagnosticEvent;
typedef struct { uint32_t reason, time_ms, epoch, session, seq; } HidDiagnosticStop;
typedef struct {
    uint32_t validity;
    HidDiagnosticStop stop;
    HidDiagnosticEvent submitted, completed, release_submitted, release_completed;
    uint32_t net_send_errors, usb_errors, core0_max_gap_us, core1_max_gap_us;
    uint32_t usb_unmatched_completions, usb_tracking_overruns;
} HidDiagnosticSnapshot;
typedef struct {
    HidDiagnosticSnapshot snapshot;
    bool pending, pending_tracked, pending_release, tracking_blocked;
    uint8_t pending_modifier, pending_keys[6];
    HidDiagnosticEvent pending_event;
    bool release_context_valid, release_context_submitted;
    HidDiagnosticEvent release_context;
    bool core0_seen, core1_seen;
    uint32_t core0_last_us, core1_last_us;
} HidDiagnostics;

void hid_diagnostic_encode(uint8_t out[HID_DIAGNOSTIC_SIZE], uint8_t result,
                           uint32_t token, uint32_t boot, uint32_t now_ms,
                           const HidDiagnosticSnapshot *snapshot);
#endif
