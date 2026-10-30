#include "hid_lease.h"
#include "hid_diagnostics.h"
#include <string.h>

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8u * i)); }

bool hid_lease_decode(const uint8_t *data, size_t size, HidLeaseRequest *r) {
    if (!data || !r || size < HID_LEASE_REQUEST_SIZE || memcmp(data, "ADRV", 4) ||
        (data[4] != 3 && data[4] != 4) || data[5] > (data[4] == 4 ? LEASE_WINDOW : LEASE_DIAGNOSTIC) ||
        u16(data + 10) || data[39]) return false;
    bool window = data[4] == 4 && data[5] == LEASE_WINDOW;
    if (!window && size != HID_LEASE_REQUEST_SIZE) return false;
    memset(r, 0, sizeof(*r));
    r->version = data[4];
    r->op = data[5]; r->lease_ms = u16(data + 6); r->hold_ms = u16(data + 8);
    r->token = u32(data + 12); r->boot = u32(data + 16); r->epoch = u32(data + 20);
    r->session = u32(data + 24); r->seq = u32(data + 28); r->modifier = data[32];
    memcpy(r->keys, data + 33, 6);
    bool keys = r->modifier != 0;
    for (unsigned i = 0; i < 6; ++i) {
        if (r->keys[i] && (r->keys[i] < 4 || r->keys[i] > 0xdf)) return false;
        if (r->keys[i]) {
            keys = true;
            for (unsigned j = 0; j < i; ++j) if (r->keys[i] == r->keys[j]) return false;
        }
    }
    if (r->op == LEASE_SET || r->op == LEASE_RENEW || r->op == LEASE_OPEN || window) {
        if (r->lease_ms < HID_LEASE_MIN_MS || r->lease_ms > HID_LEASE_MAX_MS) return false;
    } else if (r->lease_ms) return false;
    if (r->op != LEASE_SET && (keys || r->hold_ms)) return false;
    if (r->hold_ms && (r->hold_ms < 20 || r->hold_ms > 200)) return false;
    if (r->op == LEASE_OPEN && (!r->session || r->seq)) return false;
    if (r->op == LEASE_DIAGNOSTIC && (r->boot || r->epoch || r->session || r->seq)) return false;
    if (window) {
        if (size < HID_MOTION_HEADER_SIZE || u16(data + 50)) return false;
        r->revision=u32(data+40); r->event_id=u32(data+44); r->count=u16(data+48);
        r->not_after_ms=u32(data+52);
        if (!r->revision || !r->count || r->count>HID_MOTION_CAPACITY ||
            size != HID_MOTION_HEADER_SIZE + r->count * HID_MOTION_FRAME_SIZE) return false;
        unsigned total=0;
        for (unsigned i=0;i<r->count;++i) {
            const uint8_t *p=data+HID_MOTION_HEADER_SIZE+i*HID_MOTION_FRAME_SIZE;
            HidKeyframe *f=&r->frames[i]; f->duration_ms=u16(p); f->modifier=p[2];
            memcpy(f->keys,p+3,6);
            if (f->duration_ms<20 || f->duration_ms>200 || p[9]) return false;
            total+=f->duration_ms;
            for (unsigned j=0;j<6;++j) {
                if (f->keys[j] && (f->keys[j]<4 || f->keys[j]>0xdf)) return false;
                for (unsigned k=0;k<j;++k) if (f->keys[j] && f->keys[j]==f->keys[k]) return false;
            }
        }
        if (total>200) return false;
    }
    return true;
}

bool hid_lease_same_request(const HidLeaseRequest *a, const HidLeaseRequest *b) {
    return a->version == b->version && a->op == b->op && a->lease_ms == b->lease_ms && a->hold_ms == b->hold_ms &&
        a->boot == b->boot && a->epoch == b->epoch && a->session == b->session &&
        a->seq == b->seq && a->modifier == b->modifier && !memcmp(a->keys, b->keys, 6) &&
        a->revision == b->revision && a->event_id == b->event_id && a->not_after_ms == b->not_after_ms &&
        a->count == b->count && !memcmp(a->frames,b->frames,sizeof(a->frames));
}

void hid_lease_encode(uint8_t out[HID_LEASE_RESPONSE_SIZE], const HidLeaseStatus *s) {
    memset(out, 0, HID_LEASE_RESPONSE_SIZE);
    memcpy(out, "ADRS", 4); out[4] = 3; out[5] = s->result; out[6] = s->flags;
    w32(out + 8, s->token); w32(out + 12, s->boot); w32(out + 16, s->epoch);
    w32(out + 20, s->session); w32(out + 24, s->accepted_seq); w32(out + 28, s->applied_seq);
    w32(out + 32, s->uptime_ms); w16(out + 36, s->lease_remaining_ms);
    w16(out + 38, s->hold_remaining_ms); out[40] = s->modifier; memcpy(out + 41, s->keys, 6);
}


static void event_encode(uint8_t *out, const HidDiagnosticEvent *e) {
    w32(out,e->epoch); w32(out+4,e->session); w32(out+8,e->seq); w32(out+12,e->time_ms);
}
void hid_diagnostic_encode(uint8_t out[HID_DIAGNOSTIC_SIZE], uint8_t result,
                           uint32_t token, uint32_t boot, uint32_t now_ms,
                           const HidDiagnosticSnapshot *s) {
    memset(out,0,HID_DIAGNOSTIC_SIZE); memcpy(out,"ADRD",4);
    out[4]=1; out[5]=result; w16(out+6,HID_DIAGNOSTIC_SIZE);
    w32(out+8,token); w32(out+12,boot); w32(out+16,now_ms);
    w32(out+20,HID_FIRMWARE_VERSION); w32(out+24,HID_CAPABILITIES);
    w16(out+28,HID_LEASE_MIN_MS); w16(out+30,HID_LEASE_MAX_MS);
    w16(out+32,20); w16(out+34,200); w32(out+36,s->validity);
    w32(out+40,s->stop.reason); w32(out+44,s->stop.time_ms); w32(out+48,s->stop.epoch);
    w32(out+52,s->stop.session); w32(out+56,s->stop.seq);
    event_encode(out+60,&s->submitted); event_encode(out+76,&s->completed);
    event_encode(out+92,&s->release_submitted); event_encode(out+108,&s->release_completed);
    w32(out+124,s->net_send_errors); w32(out+128,s->usb_errors);
    w32(out+132,s->core0_max_gap_us); w32(out+136,s->core1_max_gap_us);
    w32(out+140,s->usb_unmatched_completions); w32(out+144,s->usb_tracking_overruns);
    w32(out+148,0x00040003u); /* minimum/maximum ADRV action protocol. */
}
