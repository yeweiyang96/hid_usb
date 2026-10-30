#ifndef HID_LEASE_H
#define HID_LEASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HID_LEASE_REQUEST_SIZE 40u
#define HID_LEASE_RESPONSE_SIZE 48u
#define HID_LEASE_MIN_MS 100u
#define HID_LEASE_MAX_MS 300u
#define HID_MOTION_CAPACITY 16u
#define HID_MOTION_RESPONSE_SIZE 80u
#define HID_MOTION_HEADER_SIZE 56u
#define HID_MOTION_FRAME_SIZE 10u
#define HID_MOTION_PACKET_SIZE (HID_MOTION_HEADER_SIZE + HID_MOTION_CAPACITY * HID_MOTION_FRAME_SIZE)

enum { LEASE_QUERY = 0, LEASE_OPEN = 1, LEASE_SET = 2, LEASE_STOP = 3, LEASE_RENEW = 4, LEASE_DIAGNOSTIC = 5 };
enum { LEASE_WINDOW = 6 };
enum { LEASE_OK = 0, LEASE_INVALID = 1, LEASE_STALE = 2, LEASE_INACTIVE = 3,
       LEASE_UNAVAILABLE = 4, LEASE_OLD_SEQUENCE = 5 };
enum { LEASE_USB = 1, LEASE_WIFI = 2, LEASE_ACTIVE = 4, LEASE_PENDING = 8,
       LEASE_HOLD_EXPIRED = 16, LEASE_CAN_OPEN = 32 };

typedef struct {
    uint16_t duration_ms;
    uint8_t modifier, keys[6];
} HidKeyframe;

typedef struct {
    uint8_t version, op, modifier, keys[6];
    uint16_t lease_ms, hold_ms;
    uint32_t token, boot, epoch, session, seq;
    uint32_t revision, event_id, not_after_ms;
    uint16_t count;
    HidKeyframe frames[HID_MOTION_CAPACITY];
} HidLeaseRequest;

typedef struct {
    uint8_t result, flags, modifier, keys[6];
    uint16_t lease_remaining_ms, hold_remaining_ms;
    uint32_t token, boot, epoch, session, accepted_seq, applied_seq, uptime_ms;
} HidLeaseStatus;

/* Protected exclusively by the bridge's lock. Actual means that TinyUSB
 * accepted a keyboard report, never that the application consumed the input. */
typedef struct {
    bool active, release_pending, desired_pending, hold_started, hold_expired;
    uint8_t version;
    uint32_t boot, session, accepted_seq, applied_seq, target_seq, expiry_ms, hold_deadline_ms;
    uint32_t closed_epoch, closed_session;
    uint16_t hold_ms;
    uint8_t modifier, keys[6], actual_modifier, actual_keys[6];
    HidLeaseRequest last_request;
} HidLease;

typedef struct {
    HidKeyframe frames[HID_MOTION_CAPACITY];
    uint8_t count, index;
    bool active, started, frame_pending;
    uint32_t revision, last_revision, event_id, last_event, applied_event;
    uint32_t not_after_ms, start_ms, due_ms, ready_deadline_ms, submitted_revision;
    uint32_t submitted_frame, completed_revision, completed_frame;
    uint32_t submitted_seq, submit_time_ms;
} HidMotion;

bool hid_lease_decode(const uint8_t *data, size_t size, HidLeaseRequest *request);
void hid_lease_encode(uint8_t out[HID_LEASE_RESPONSE_SIZE], const HidLeaseStatus *status);
bool hid_lease_same_request(const HidLeaseRequest *a, const HidLeaseRequest *b);

#endif
