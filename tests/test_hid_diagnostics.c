#include "hid_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(v) do { if (!(v)) { fprintf(stderr,"FAIL diagnostic line %d: %s\n",__LINE__,#v); exit(1); } } while(0)
typedef struct { unsigned calls; uint8_t modifier,keys[6]; } Output;
static bool output(void *context,uint8_t mod,const uint8_t keys[6]) {
    Output *o=context; ++o->calls; o->modifier=mod; memcpy(o->keys,keys,6); return true;
}
static void w16(uint8_t *p,uint16_t v) { p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8); }
static void w32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8u*i)); }
static void encode(HidLeaseRequest *r,uint8_t p[40]) {
    memset(p,0,40);memcpy(p,"ADRV",4);p[4]=3;p[5]=r->op;w16(p+6,r->lease_ms);w16(p+8,r->hold_ms);
    w32(p+12,r->token);w32(p+16,r->boot);w32(p+20,r->epoch);w32(p+24,r->session);w32(p+28,r->seq);
    p[32]=r->modifier;memcpy(p+33,r->keys,6);
}
static uint8_t request(HidBridge *b,HidLeaseRequest *r,uint32_t now) {
    uint8_t p[40],out[48];encode(r,p);hid_bridge_lease_request(b,p,40,now,out);return out[5];
}
static void tick(HidBridge *b,Output *o,uint32_t now) {
    hid_bridge_publish_at(b,hid_bridge_epoch(b),7,true,false,0,(uint64_t)now*1000u);
    hid_bridge_lease_tick(b,now,output,o);
}
static void complete(HidBridge *b,Output *o,uint32_t now) {
    hid_bridge_keyboard_complete(b,o->modifier,o->keys,now,true);
}
static HidLeaseRequest start(HidBridge *b,Output *o) {
    hid_bridge_init(b);hid_bridge_set_boot(b,123);
    hid_bridge_publish_at(b,hid_bridge_epoch(b),7,true,false,0,1000000);
    hid_bridge_network_at(b,true,false,1000);
    HidLeaseRequest r={0};r.op=LEASE_OPEN;r.boot=123;r.epoch=1;r.session=42;r.lease_ms=250;
    CHECK(request(b,&r,1000)==LEASE_OK);tick(b,o,1001);complete(b,o,1002);
    r.op=LEASE_SET;r.epoch=2;r.seq=1;r.keys[0]=26;
    return r;
}
static HidLeaseRequest stop_for(HidLeaseRequest r) {
    r.op=LEASE_STOP;r.lease_ms=r.hold_ms=r.modifier=0;memset(r.keys,0,6);return r;
}
static void diag(HidBridge *b,uint32_t now,uint8_t out[160]) {
    HidLeaseRequest r={0};r.op=LEASE_DIAGNOSTIC;r.token=0x12345678;
    uint8_t p[40];encode(&r,p);hid_bridge_diagnostic_request(b,p,40,now,out);
}

static void test_wire_and_read_only(void) {
    HidBridge b;Output o={0};HidLeaseRequest r=start(&b,&o);
    CHECK(request(&b,&r,1003)==LEASE_OK);tick(&b,&o,1004);
    uint32_t expiry=b.lease.expiry_ms,seq=b.lease.accepted_seq;unsigned reports=o.calls;
    uint8_t out[160];diag(&b,1100,out);
    CHECK(!memcmp(out,"ADRD",4)&&out[4]==1&&out[5]==0&&out[6]==160&&out[7]==0);
    CHECK(hid_read_u32(out+8)==0x12345678&&hid_read_u32(out+12)==123&&hid_read_u32(out+16)==1100);
    CHECK(hid_read_u32(out+20)==0x00060000&&hid_read_u32(out+24)==0x7f);
    CHECK(hid_read_u32(out+28)==0x012c0064&&hid_read_u32(out+32)==0x00c80014);
    CHECK(hid_read_u32(out+148)==0x00040003&&hid_read_u32(out+152)==0&&hid_read_u32(out+156)==0);
    CHECK(b.lease.expiry_ms==expiry&&b.lease.accepted_seq==seq&&o.calls==reports);
    CHECK(hid_read_u32(out+36)&HID_VALID_SUBMITTED);
    CHECK(hid_read_u32(out+60)==2&&hid_read_u32(out+64)==42&&hid_read_u32(out+68)==1&&hid_read_u32(out+72)==1004);
    HidLeaseRequest bad={0};bad.op=LEASE_DIAGNOSTIC;bad.token=5;bad.session=1;
    uint8_t p[40];encode(&bad,p);hid_bridge_diagnostic_request(&b,p,40,1101,out);CHECK(out[5]==LEASE_INVALID);
    CHECK(b.lease.expiry_ms==expiry&&o.calls==reports);
    puts("PASS diagnostic 160-byte layout, version/capabilities/ranges, validity and read-only behavior");
}

static void test_submit_complete_and_stop_identity(void) {
    HidBridge b;Output o={0};HidLeaseRequest r=start(&b,&o);
    CHECK(request(&b,&r,1003)==LEASE_OK);tick(&b,&o,1004);
    HidDiagnosticSnapshot *s=&b.diagnostics.snapshot;
    CHECK((s->validity&HID_VALID_SUBMITTED)&&!(s->validity&HID_VALID_COMPLETED));
    CHECK(s->submitted.time_ms==1004&&s->submitted.seq==1);
    HidLeaseRequest stop=stop_for(r);CHECK(request(&b,&stop,1010)==LEASE_OK);
    CHECK(s->stop.reason==HID_STOP_CLIENT&&s->stop.time_ms==1010&&s->stop.epoch==2&&s->stop.session==42&&s->stop.seq==1);
    complete(&b,&o,1011); /* Old W completes after cancellation, still old identity. */
    CHECK(s->completed.epoch==2&&s->completed.session==42&&s->completed.seq==1&&s->completed.time_ms==1011);
    tick(&b,&o,1012);CHECK(!o.keys[0]&&(s->validity&HID_VALID_RELEASE_SUBMITTED));
    CHECK(s->release_submitted.epoch==2&&s->release_submitted.time_ms==1012);
    CHECK(!(s->validity&HID_VALID_RELEASE_COMPLETED));
    /* A new session can be admitted before the old release completion callback.
     * The callback must still be associated with the old released session. */
    HidLeaseRequest next={0};next.op=LEASE_OPEN;next.boot=123;next.epoch=3;next.session=99;next.lease_ms=250;
    CHECK(request(&b,&next,1013)==LEASE_OK);
    complete(&b,&o,1014);
    CHECK(s->release_completed.epoch==2&&s->release_completed.session==42&&s->release_completed.seq==1&&s->release_completed.time_ms==1014);
    CHECK(request(&b,&stop,1015)==LEASE_STALE);
    CHECK(s->stop.reason==HID_STOP_CLIENT&&s->stop.time_ms==1010);
    puts("PASS submission is not completion; cancellation and new sessions cannot rename an in-flight completion");
}

static void test_first_stop_reasons(void) {
    for(unsigned reason=1;reason<=8;++reason) {
        HidBridge b;Output o={0};HidLeaseRequest r=start(&b,&o);
        CHECK(request(&b,&r,1003)==LEASE_OK);tick(&b,&o,1004);complete(&b,&o,1005);
        uint32_t expected=1010;
        if(reason==HID_STOP_CLIENT) { HidLeaseRequest stop=stop_for(r);CHECK(request(&b,&stop,expected)==LEASE_OK); }
        else if(reason==HID_STOP_EXPIRED) {expected=1253;hid_bridge_lease_tick(&b,expected,output,&o);}
        else if(reason==HID_STOP_WIFI) hid_bridge_network_at(&b,false,false,expected);
        else if(reason==HID_STOP_USB_UNAVAILABLE) hid_bridge_publish_at(&b,2,0,false,false,0,(uint64_t)expected*1000u);
        else if(reason==HID_STOP_LEGACY_RELEASE) { b.clock_ms=1;hid_bridge_release_at(&b,expected); }
        else hid_bridge_cancel_reason(&b,reason,expected);
        HidDiagnosticSnapshot *s=&b.diagnostics.snapshot;
        CHECK(s->stop.reason==reason&&s->stop.time_ms==expected&&s->stop.epoch==2&&s->stop.session==42&&s->stop.seq==1);
        HidLeaseRequest stop=stop_for(r);CHECK(request(&b,&stop,expected+1)==LEASE_OK);
        hid_bridge_network_at(&b,false,true,expected+2);
        CHECK(s->stop.reason==reason&&s->stop.time_ms==expected);
    }
    puts("PASS first client/expiry/Wi-Fi/USB/legacy stop reason survives retries and later disconnects");
}

static void test_failure_and_overwrite_never_complete(void) {
    HidBridge b;Output o={0};HidLeaseRequest r=start(&b,&o);
    CHECK(request(&b,&r,1003)==LEASE_OK);tick(&b,&o,1004);
    ++r.seq;CHECK(request(&b,&r,1005)==LEASE_OK);tick(&b,&o,1006); /* impossible real endpoint overlap */
    CHECK(b.diagnostics.snapshot.usb_tracking_overruns==1&&b.diagnostics.tracking_blocked);
    complete(&b,&o,1007); /* identical W bytes cannot identify either SET. */
    CHECK(!(b.diagnostics.snapshot.validity&HID_VALID_COMPLETED));
    ++r.seq;CHECK(request(&b,&r,1008)==LEASE_OK);tick(&b,&o,1009);complete(&b,&o,1010);
    CHECK(!(b.diagnostics.snapshot.validity&HID_VALID_COMPLETED));
    hid_bridge_cancel_reason(&b,HID_STOP_USB_FAILED,1011);
    CHECK(b.diagnostics.snapshot.usb_errors==1&&!b.diagnostics.pending);
    complete(&b,&o,1012);CHECK(!(b.diagnostics.snapshot.validity&HID_VALID_COMPLETED));
    CHECK(b.diagnostics.snapshot.usb_unmatched_completions==1);
    /* USB removal also invalidates the pending record without a completion. */
    r=start(&b,&o);CHECK(request(&b,&r,1003)==LEASE_OK);tick(&b,&o,1004);
    hid_bridge_publish_at(&b,2,0,false,false,0,1005000);
    complete(&b,&o,1006);CHECK(!(b.diagnostics.snapshot.validity&HID_VALID_COMPLETED));
    puts("PASS overwritten identical reports, failed transfers and unplug do not create completion timestamps");
}

static void test_counters_gaps_wrap(void) {
    HidBridge b;hid_bridge_init(&b);
    uint64_t start_us=UINT32_MAX-10u;
    hid_bridge_publish_at(&b,1,7,true,false,0,start_us);
    hid_bridge_network_heartbeat_at(&b,start_us);
    CHECK(!b.diagnostics.snapshot.core0_max_gap_us&&!b.diagnostics.snapshot.core1_max_gap_us);
    hid_bridge_publish_at(&b,1,7,true,false,0,start_us+35);
    hid_bridge_network_heartbeat_at(&b,start_us+50);
    CHECK(b.diagnostics.snapshot.core0_max_gap_us==35&&b.diagnostics.snapshot.core1_max_gap_us==50);
    hid_bridge_publish_at(&b,1,7,true,false,0,start_us+40);
    CHECK(b.diagnostics.snapshot.core0_max_gap_us==35);
    hid_bridge_note_send_error(&b);CHECK(b.diagnostics.snapshot.net_send_errors==1);
    b.diagnostics.snapshot.net_send_errors=UINT32_MAX;hid_bridge_note_send_error(&b);
    CHECK(b.diagnostics.snapshot.net_send_errors==0);
    puts("PASS first loop excluded, microsecond gap wrap/max and uint32 diagnostic counters");
}

static void test_finite_hold_release_and_time_wrap(void) {
    HidBridge b;Output o={0};HidLeaseRequest r=start(&b,&o);
    r.hold_ms=20;CHECK(request(&b,&r,1003)==LEASE_OK);tick(&b,&o,1004);complete(&b,&o,1005);
    tick(&b,&o,1024);
    HidDiagnosticSnapshot *s=&b.diagnostics.snapshot;
    CHECK(s->release_submitted.seq==1&&s->release_submitted.time_ms==1024);
    CHECK(!(s->validity&HID_VALID_RELEASE_COMPLETED));
    complete(&b,&o,1025);CHECK(s->release_completed.time_ms==1025&&s->release_completed.epoch==2);
    CHECK(!(s->validity&HID_VALID_STOP));
    ++r.seq;r.hold_ms=0;CHECK(request(&b,&r,1030)==LEASE_OK);tick(&b,&o,1031);
    b.diagnostics.pending_event.time_ms=UINT32_MAX-1u;
    complete(&b,&o,1);
    CHECK(s->completed.time_ms==1&&(uint32_t)(s->completed.time_ms-(UINT32_MAX-1u))==3);
    puts("PASS finite hold release has real separate callback time and timestamp wrap preserves valid zero/low values");
}

int main(void) {
    test_wire_and_read_only();test_submit_complete_and_stop_identity();test_first_stop_reasons();
    test_failure_and_overwrite_never_complete();test_counters_gaps_wrap();test_finite_hold_release_and_time_wrap();
    puts("All six production diagnostic groups passed.");return 0;
}
