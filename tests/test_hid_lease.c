#include "hid_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(v) do { if (!(v)) { fprintf(stderr, "FAIL lease line %d: %s\n", __LINE__, #v); exit(1); } } while (0)

typedef struct { unsigned calls, zeros; bool ready; uint8_t modifier, keys[6]; } Output;
static bool output(void *context, uint8_t modifier, const uint8_t keys[6]) {
    Output *o = context;
    if (!o->ready) return false;
    ++o->calls; o->modifier = modifier; memcpy(o->keys, keys, 6);
    bool zero = !modifier;
    for (unsigned i = 0; i < 6; ++i) zero = zero && !keys[i];
    if (zero) ++o->zeros;
    return true;
}
static void w16(uint8_t *p, uint16_t value) { p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); }
static void w32(uint8_t *p, uint32_t value) { for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (i * 8)); }
static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static void encode_request(const HidLeaseRequest *r, uint8_t data[40]) {
    memset(data, 0, 40); memcpy(data, "ADRV", 4); data[4] = 3; data[5] = r->op;
    w16(data + 6, r->lease_ms); w16(data + 8, r->hold_ms); w32(data + 12, r->token);
    w32(data + 16, r->boot); w32(data + 20, r->epoch); w32(data + 24, r->session);
    w32(data + 28, r->seq); data[32] = r->modifier; memcpy(data + 33, r->keys, 6);
}
static HidLeaseStatus request(HidBridge *b, HidLeaseRequest *r, uint32_t now) {
    uint8_t data[40], reply[48]; encode_request(r, data);
    hid_bridge_lease_request(b, data, sizeof(data), now, reply);
    CHECK(!memcmp(reply, "ADRS", 4) && reply[4] == 3 && !reply[7] && !reply[47]);
    HidLeaseStatus s = {0};
    s.result = reply[5]; s.flags = reply[6]; s.token = hid_read_u32(reply + 8);
    s.boot = hid_read_u32(reply + 12); s.epoch = hid_read_u32(reply + 16);
    s.session = hid_read_u32(reply + 20); s.accepted_seq = hid_read_u32(reply + 24);
    s.applied_seq = hid_read_u32(reply + 28); s.uptime_ms = hid_read_u32(reply + 32);
    s.lease_remaining_ms = r16(reply + 36); s.hold_remaining_ms = r16(reply + 38);
    s.modifier = reply[40]; memcpy(s.keys, reply + 41, 6);
    CHECK(s.token == r->token);
    return s;
}
static HidLeaseStatus query(HidBridge *b, uint32_t now) {
    HidLeaseRequest r = {0}; r.token = 0x12345678;
    return request(b, &r, now);
}
static void ready(HidBridge *b) {
    hid_bridge_init(b); hid_bridge_set_boot(b, 0x1234);
    hid_bridge_publish(b, hid_bridge_epoch(b), 7, true, false, 0);
    hid_bridge_network(b, true, false);
}
static void ack(HidBridge *b) {
    hid_bridge_publish(b, hid_bridge_epoch(b), 7, true, false, 0);
}
static HidLeaseRequest open_session(HidBridge *b, Output *o, uint32_t now) {
    HidLeaseStatus s = query(b, now);
    CHECK(s.flags & LEASE_CAN_OPEN);
    HidLeaseRequest r = {0};
    r.op = LEASE_OPEN; r.lease_ms = 250; r.token = 9; r.boot = s.boot; r.epoch = s.epoch; r.session = 42;
    HidLeaseStatus opened = request(b, &r, now);
    CHECK(opened.result == LEASE_OK && opened.epoch == s.epoch + 1);
    CHECK(opened.flags & LEASE_ACTIVE);
    CHECK(opened.flags & LEASE_PENDING);
    unsigned count = o->calls;
    hid_bridge_lease_tick(b, now, output, o);
    CHECK(o->calls == count); /* Core 0 has not acknowledged its release yet. */
    ack(b); hid_bridge_lease_tick(b, now, output, o);
    CHECK(o->zeros && !(query(b, now).flags & LEASE_PENDING));
    r.epoch = opened.epoch; r.op = LEASE_SET; r.seq = 1; r.keys[0] = 26;
    return r;
}

static void test_wire(void) {
    HidLeaseRequest r = {0}, decoded;
    r.op = LEASE_SET; r.lease_ms = 250; r.hold_ms = 100; r.token = 0x12345678;
    r.boot = 0x10203040; r.epoch = 12; r.session = 0x76543210; r.seq = 9;
    r.modifier = 2; r.keys[0] = 26;
    uint8_t data[40]; encode_request(&r, data);
    const uint8_t golden[40] = {65,68,82,86,3,2,250,0,100,0,0,0,0x78,0x56,0x34,0x12,
        0x40,0x30,0x20,0x10,12,0,0,0,0x10,0x32,0x54,0x76,9,0,0,0,2,26,0,0,0,0,0,0};
    CHECK(!memcmp(data, golden, 40));
    CHECK(hid_lease_decode(data, 40, &decoded) && hid_lease_same_request(&r, &decoded));
    for (unsigned i = 0; i < 40; ++i) CHECK(!hid_lease_decode(data, i, &decoded));
    data[39] = 1; CHECK(!hid_lease_decode(data, 40, &decoded)); data[39] = 0;
    data[10] = 1; CHECK(!hid_lease_decode(data, 40, &decoded)); data[10] = 0;
    data[34] = 26; CHECK(!hid_lease_decode(data, 40, &decoded)); data[34] = 0;
    data[33] = 0xe0; CHECK(!hid_lease_decode(data, 40, &decoded)); data[33] = 26;
    w16(data + 6, 99); CHECK(!hid_lease_decode(data, 40, &decoded));
    w16(data + 6, 301); CHECK(!hid_lease_decode(data, 40, &decoded));
    w16(data + 6, 250); w16(data + 8, 19); CHECK(!hid_lease_decode(data, 40, &decoded));
    w16(data + 8, 201); CHECK(!hid_lease_decode(data, 40, &decoded));
    HidLeaseStatus status = {0};
    status.result = 5; status.flags = 7; status.token = 0x12345678; status.boot = 0x10203040;
    status.epoch = 12; status.session = 0x76543210; status.accepted_seq = 9; status.applied_seq = 8;
    status.uptime_ms = 1000; status.lease_remaining_ms = 250; status.hold_remaining_ms = 100;
    status.modifier = 2; status.keys[0] = 26;
    uint8_t reply[48]; hid_lease_encode(reply, &status);
    const uint8_t expected[48] = {65,68,82,83,3,5,7,0,0x78,0x56,0x34,0x12,
        0x40,0x30,0x20,0x10,12,0,0,0,0x10,0x32,0x54,0x76,9,0,0,0,8,0,0,0,
        0xe8,3,0,0,250,0,100,0,2,26,0,0,0,0,0,0};
    CHECK(!memcmp(reply, expected, 48));
    puts("PASS lease exact 40/48-byte golden request/response, invalid fields and key validation");
}

static void test_continuous(void) {
    HidBridge b; ready(&b); Output o = {0}; o.ready = true;
    HidLeaseRequest r = open_session(&b, &o, 1000);
    CHECK(request(&b, &r, 1001).result == LEASE_OK);
    hid_bridge_lease_tick(&b, 1001, output, &o);
    CHECK(o.keys[0] == 26 && o.calls == 2 && o.zeros == 1);
    r.op = LEASE_RENEW; memset(r.keys, 0, 6);
    for (uint32_t now = 1050; now < 2000; now += 50) {
        ++r.seq;
        HidLeaseStatus s = request(&b, &r, now);
        CHECK(s.result == LEASE_OK && s.applied_seq == 1 && s.lease_remaining_ms == 250);
        hid_bridge_lease_tick(&b, now, output, &o);
        CHECK(o.keys[0] == 26 && o.calls == 2 && o.zeros == 1);
    }
    /* Pretend SET(seq+1, a different key) was dropped; a later renewal must
     * not falsely acknowledge that missing state change. */
    r.seq += 2;
    HidLeaseStatus after_drop = request(&b, &r, 1951);
    CHECK(after_drop.accepted_seq == r.seq && after_drop.applied_seq == 1 && after_drop.keys[0] == 26);
    uint32_t epoch = r.epoch;
    hid_bridge_lease_tick(&b, 2201, output, &o);
    CHECK(!(query(&b, 2201).flags & LEASE_ACTIVE) && hid_bridge_epoch(&b) == epoch + 1);
    ack(&b); hid_bridge_lease_tick(&b, 2201, output, &o);
    CHECK(o.keys[0] == 0 && o.zeros == 2);
    ++r.seq; CHECK(request(&b, &r, 2201).result == LEASE_STALE);
    puts("PASS continuous renew has no key-up transitions and lease expiry invalidates late renewal");
}

static void test_duplicates_and_order(void) {
    HidBridge b; ready(&b); Output o = {0}; o.ready = true;
    HidLeaseRequest r = open_session(&b, &o, 1000);
    CHECK(request(&b, &r, 1001).result == LEASE_OK);
    hid_bridge_lease_tick(&b, 1001, output, &o);
    HidLeaseRequest duplicate = r;
    CHECK(request(&b, &duplicate, 1100).lease_remaining_ms == 151);
    duplicate.keys[0] = 4;
    CHECK(request(&b, &duplicate, 1110).result == LEASE_OLD_SEQUENCE);
    r.op = LEASE_RENEW; memset(r.keys, 0, 6); ++r.seq;
    CHECK(request(&b, &r, 1120).result == LEASE_OK);
    CHECK(request(&b, &duplicate, 1130).result == LEASE_OLD_SEQUENCE);
    HidLeaseRequest old_open = r;
    old_open.op = LEASE_OPEN; old_open.seq = 0; --old_open.epoch;
    CHECK(request(&b, &old_open, 1200).result == LEASE_OK);
    CHECK(query(&b, 1200).lease_remaining_ms == 170); /* Neither duplicate extends TTL. */
    hid_bridge_lease_tick(&b, 1370, output, &o); ack(&b); hid_bridge_lease_tick(&b, 1370, output, &o);
    CHECK(request(&b, &old_open, 1380).result == LEASE_STALE);
    CHECK(request(&b, &r, 1380).result == LEASE_STALE);
    CHECK(o.keys[0] == 0);
    puts("PASS duplicates, mismatched duplicates, reordered packets and delayed OPEN never extend expired authorization");
}

static void test_hold_and_backpressure(void) {
    HidBridge b; ready(&b); Output o = {0}; o.ready = true;
    HidLeaseRequest r = open_session(&b, &o, 1000);
    r.keys[0] = 4; r.hold_ms = 100;
    CHECK(request(&b, &r, 1001).result == LEASE_OK);
    o.ready = false; hid_bridge_lease_tick(&b, 1010, output, &o);
    HidLeaseStatus s = query(&b, 1010);
    CHECK(s.accepted_seq == 1 && s.applied_seq == 0 && !s.hold_remaining_ms && (s.flags & LEASE_PENDING));
    HidLeaseRequest early_renew = r;
    early_renew.op = LEASE_RENEW; early_renew.hold_ms = 0; memset(early_renew.keys, 0, 6); ++early_renew.seq;
    CHECK(request(&b, &early_renew, 1020).applied_seq == 0);
    o.ready = true; hid_bridge_lease_tick(&b, 1040, output, &o);
    CHECK(query(&b, 1040).applied_seq == 1); /* A pending SET is not renamed by renewal. */
    r.seq = 2;
    CHECK(query(&b, 1040).hold_remaining_ms == 100 && o.keys[0] == 4);
    r.op = LEASE_RENEW; r.hold_ms = 0; memset(r.keys, 0, 6); ++r.seq;
    CHECK(request(&b, &r, 1100).hold_remaining_ms == 40);
    o.ready = false; hid_bridge_lease_tick(&b, 1140, output, &o);
    s = query(&b, 1140);
    CHECK((s.flags & (LEASE_ACTIVE | LEASE_HOLD_EXPIRED | LEASE_PENDING)) ==
        (LEASE_ACTIVE | LEASE_HOLD_EXPIRED | LEASE_PENDING));
    CHECK(s.keys[0] == 4); /* A requested release is not an applied release. */
    o.ready = true; hid_bridge_lease_tick(&b, 1141, output, &o);
    s = query(&b, 1141); CHECK(!s.keys[0] && !(s.flags & LEASE_PENDING) && s.applied_seq == 1);
    ++r.seq; CHECK(request(&b, &r, 1180).flags & LEASE_HOLD_EXPIRED);
    hid_bridge_lease_tick(&b, 1180, output, &o); CHECK(o.keys[0] == 0);
    r.op = LEASE_SET; r.keys[0] = 7; r.hold_ms = 100; ++r.seq;
    CHECK(request(&b, &r, 1190).result == LEASE_OK);
    hid_bridge_lease_tick(&b, 1190, output, &o);
    CHECK(o.keys[0] == 7 && !(query(&b, 1190).flags & LEASE_HOLD_EXPIRED));
    puts("PASS finite hold starts at USB submission; renew cannot restart it; release backpressure remains visible");
}

static void test_stop_and_legacy(void) {
    HidBridge b; ready(&b); Output o = {0}; o.ready = true;
    HidLeaseRequest r = open_session(&b, &o, 1000);
    CHECK(request(&b, &r, 1001).result == LEASE_OK);
    hid_bridge_lease_tick(&b, 1001, output, &o);
    HidCommand legacy = {0}; legacy.type = CMD_PRESS_KEY; legacy.code = 26; legacy.hold_ms = 5000;
    CHECK(!hid_bridge_submit(&b, &legacy));
    HidLeaseRequest stop = r; stop.op = LEASE_STOP; stop.lease_ms = stop.hold_ms = 0; memset(stop.keys, 0, 6);
    CHECK(request(&b, &stop, 1010).result == LEASE_OK);
    CHECK(query(&b, 1010).flags & LEASE_PENDING);
    CHECK(request(&b, &stop, 1011).result == LEASE_OK);
    ack(&b); hid_bridge_lease_tick(&b, 1011, output, &o);
    CHECK(!o.keys[0] && (query(&b, 1011).flags & LEASE_CAN_OPEN));
    HidLeaseRequest new_session = open_session(&b, &o, 1020);
    CHECK(request(&b, &stop, 1021).result == LEASE_STALE);
    CHECK(query(&b, 1021).flags & LEASE_ACTIVE);
    CHECK(request(&b, &new_session, 1022).result == LEASE_OK);
    hid_bridge_lease_tick(&b, 1022, output, &o); CHECK(o.keys[0] == 26);
    legacy.type = CMD_RELEASE_ALL; CHECK(hid_bridge_submit(&b, &legacy));
    CHECK(!(query(&b, 1023).flags & LEASE_ACTIVE));
    ack(&b); hid_bridge_lease_tick(&b, 1023, output, &o); CHECK(!o.keys[0]);
    for (unsigned i = 0; i < 8; ++i) { legacy.type = CMD_PRESS_KEY; CHECK(hid_bridge_submit(&b, &legacy)); }
    legacy.type = CMD_RELEASE_ALL; CHECK(hid_bridge_submit(&b, &legacy));
    CHECK(!hid_bridge_status(&b).queued);
    puts("PASS STOP and legacy release preempt; active leases exclude legacy actions; old stop cannot cancel a new session");
}

static void test_disconnect_reset_and_wrap(void) {
    HidBridge b; ready(&b); Output o = {0}; o.ready = true;
    HidLeaseRequest r = open_session(&b, &o, UINT32_MAX - 100u);
    CHECK(request(&b, &r, UINT32_MAX - 90u).result == LEASE_OK);
    hid_bridge_lease_tick(&b, UINT32_MAX - 90u, output, &o);
    CHECK(query(&b, 50).lease_remaining_ms == 109);
    hid_bridge_lease_tick(&b, 159, output, &o);
    CHECK(!(query(&b, 159).flags & LEASE_ACTIVE));
    ack(&b); hid_bridge_lease_tick(&b, 159, output, &o); CHECK(!o.keys[0]);
    r = open_session(&b, &o, 200);
    CHECK(request(&b, &r, 201).result == LEASE_OK);
    hid_bridge_lease_tick(&b, 201, output, &o);
    hid_bridge_network(&b, false, false); ack(&b); hid_bridge_lease_tick(&b, 202, output, &o);
    CHECK(!o.keys[0] && !(query(&b, 202).flags & LEASE_ACTIVE));
    hid_bridge_network(&b, true, false);
    CHECK(request(&b, &r, 203).result == LEASE_STALE);
    r = open_session(&b, &o, 210);
    CHECK(request(&b, &r, 211).result == LEASE_OK);
    hid_bridge_lease_tick(&b, 211, output, &o);
    hid_bridge_publish(&b, hid_bridge_epoch(&b), 0, false, false, 0);
    CHECK(!(query(&b, 212).flags & LEASE_ACTIVE));
    ack(&b); hid_bridge_lease_tick(&b, 213, output, &o); CHECK(!o.keys[0]);
    ready(&b); hid_bridge_set_boot(&b, 0x2345);
    CHECK(request(&b, &r, 214).result == LEASE_STALE);
    puts("PASS uptime wrap, USB/Wi-Fi loss and boot identity close the session without input replay");
}

static void test_epoch_sequence_wrap(void) {
    HidBridge b; ready(&b); Output o = {0}; o.ready = true;
    b.epoch = b.acknowledged_epoch = UINT32_MAX;
    HidLeaseRequest r = open_session(&b, &o, 1000);
    CHECK(r.epoch == 0);
    b.lease.accepted_seq = UINT32_MAX - 1u; /* Simulate a very long live session. */
    r.seq = UINT32_MAX;
    CHECK(request(&b, &r, 1001).result == LEASE_OK);
    hid_bridge_lease_tick(&b, 1001, output, &o);
    CHECK(query(&b, 1001).applied_seq == UINT32_MAX);
    r.seq = 0; r.keys[0] = 4;
    CHECK(request(&b, &r, 1002).result == LEASE_OK);
    hid_bridge_lease_tick(&b, 1002, output, &o);
    CHECK(query(&b, 1002).applied_seq == 0 && o.keys[0] == 4);
    r.seq = UINT32_MAX; CHECK(request(&b, &r, 1003).result == LEASE_OLD_SEQUENCE);
    r.op = LEASE_STOP; r.lease_ms = 0; memset(r.keys, 0, 6);
    CHECK(request(&b, &r, 1004).result == LEASE_OK && hid_bridge_epoch(&b) == 1);
    ack(&b); hid_bridge_lease_tick(&b, 1004, output, &o); CHECK(!o.keys[0]);
    puts("PASS epoch and SET sequence uint32 wrap remain ordered across stop");
}

int main(void) {
    test_wire(); test_continuous(); test_duplicates_and_order(); test_hold_and_backpressure();
    test_stop_and_legacy(); test_disconnect_reset_and_wrap(); test_epoch_sequence_wrap();
    puts("All seven lease protocol groups passed (production bridge and host USB transport).");
    return 0;
}
