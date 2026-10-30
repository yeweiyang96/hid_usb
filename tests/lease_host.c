/* Test adapter only. Production packet parsing and lease/bridge state machines
 * are linked unchanged. No network sockets and no real USB APIs are present. */
#include "hid_bridge.h"
#include <string.h>

#define EXPORT __declspec(dllexport)
static HidBridge bridge;
static bool usb_available, endpoint_ready;
static uint8_t last_keyboard[7];
static unsigned reports, releases;

static bool keyboard(void *context, uint8_t modifier, const uint8_t keys[6]) {
    (void)context;
    if (!endpoint_ready || !usb_available) return false;
    last_keyboard[0] = modifier; memcpy(last_keyboard + 1, keys, 6);
    ++reports;
    bool zero = modifier == 0;
    for (unsigned i = 0; i < 6; ++i) zero = zero && !keys[i];
    if (zero) ++releases;
    return true;
}

EXPORT void reset(void) {
    hid_bridge_init(&bridge); hid_bridge_set_boot(&bridge, 0x12345678);
    usb_available = endpoint_ready = true;
    reports = releases = 0; memset(last_keyboard, 0, sizeof(last_keyboard));
    hid_bridge_publish(&bridge, hid_bridge_epoch(&bridge), 7, true, false, 0);
    hid_bridge_network(&bridge, true, false);
}

EXPORT void tick(uint32_t now_ms) {
    /* The adapter has no old queued actions. Model Core 0 completing its
     * cancellation barrier, then run the real lease tick/USB decision. */
    hid_bridge_publish_at(&bridge, hid_bridge_epoch(&bridge), usb_available ? 7 : 0,
                       usb_available, false, 0,(uint64_t)now_ms*1000u);
    hid_bridge_lease_tick(&bridge, now_ms, keyboard, NULL);
}

EXPORT void exchange(const uint8_t request[40], uint32_t now_ms, uint8_t response[48]) {
    tick(now_ms);
    hid_bridge_lease_request(&bridge, request, HID_LEASE_REQUEST_SIZE, now_ms, response);
}

EXPORT void exchange_motion(const uint8_t *request,unsigned size,uint32_t now_ms,uint8_t response[80]) {
    tick(now_ms);
    hid_bridge_motion_request(&bridge,request,size,now_ms,response);
}

EXPORT void copy_keyboard(uint8_t out[7]) { memcpy(out, last_keyboard, 7); }
EXPORT unsigned keyboard_reports(void) { return reports; }
EXPORT unsigned keyboard_releases(void) { return releases; }
EXPORT void set_endpoint_ready(int ready) { endpoint_ready = ready != 0; }
EXPORT void set_usb_available(int ready) {
    usb_available = ready != 0;
    hid_bridge_publish(&bridge, hid_bridge_epoch(&bridge), ready ? 7 : 0, usb_available, false, 0);
}
EXPORT void set_network(int up) { hid_bridge_network(&bridge, up != 0, false); }


EXPORT void exchange_diagnostic(const uint8_t request[40], uint32_t now_ms, uint8_t response[160]) {
    tick(now_ms);
    hid_bridge_diagnostic_request(&bridge,request,HID_LEASE_REQUEST_SIZE,now_ms,response);
}
EXPORT void complete_keyboard(uint32_t now_ms, int success) {
    if (success) hid_bridge_keyboard_complete(&bridge,last_keyboard[0],last_keyboard+1,now_ms,true);
    else hid_bridge_cancel_reason(&bridge,HID_STOP_USB_FAILED,now_ms);
}
EXPORT void complete_report(uint32_t now_ms,uint8_t modifier,const uint8_t keys[6],int valid) {
    hid_bridge_keyboard_complete(&bridge,modifier,keys,now_ms,valid!=0);
}
EXPORT void note_send_error(void) { hid_bridge_note_send_error(&bridge); }
EXPORT void network_heartbeat_us(uint64_t now_us) { hid_bridge_network_heartbeat_at(&bridge,now_us); }
