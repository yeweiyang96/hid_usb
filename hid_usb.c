#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/rand.h"
#include "hardware/watchdog.h"
#include "tusb.h"

#include "hid_engine.h"
#include "hid_bridge.h"
#include "usb_descriptors.h"
#include "wifi_service.h"

enum { ITF_KEYBOARD = 0, ITF_MOUSE = 1 };
static HidEngine engine; /* Owned exclusively by Core 0, including callbacks. */
static HidBridge bridge;
static uint32_t engine_epoch;
static bool watchdog_boot;
/* Network callbacks and their main loop share this Core 1 stack. Using main
 * SRAM avoids overflowing the default 4 KiB SCRATCH_X stack bank. */
_Alignas(32) static uint32_t network_core_stack[0x2000u / sizeof(uint32_t)];

/* Called with the bridge lock already held, never reenters it. */
static bool lease_keyboard(void *context, uint8_t modifier, const uint8_t keys[6]) {
    (void)context;
    if (!tud_hid_n_ready(ITF_KEYBOARD)) return false;
    uint8_t id = tud_hid_n_get_protocol(ITF_KEYBOARD) == HID_PROTOCOL_BOOT ? 0 : REPORT_ID_KEYBOARD;
    return tud_hid_n_keyboard_report(ITF_KEYBOARD, id, modifier, keys);
}

static bool send_keyboard(void *context, uint8_t modifier, const uint8_t keys[6]) {
    (void)context;
    bool release = modifier == 0;
    for (unsigned i = 0; i < 6; ++i) release = release && keys[i] == 0;
    if (!hid_bridge_output_begin(&bridge, engine_epoch, release)) return false;
    bool sent = false;
    if (tud_hid_n_ready(ITF_KEYBOARD)) {
        uint8_t id = tud_hid_n_get_protocol(ITF_KEYBOARD) == HID_PROTOCOL_BOOT ? 0 : REPORT_ID_KEYBOARD;
        sent = tud_hid_n_keyboard_report(ITF_KEYBOARD, id, modifier, keys);
        if (sent) hid_bridge_keyboard_submitted_locked(&bridge,modifier,keys,
                          (uint32_t)(time_us_64()/1000u),false);
    }
    hid_bridge_output_end(&bridge);
    return sent;
}

static bool send_mouse(void *context, uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel) {
    (void)context;
    bool release = buttons == 0 && dx == 0 && dy == 0 && wheel == 0;
    if (!hid_bridge_output_begin(&bridge, engine_epoch, release)) return false;
    bool sent = false;
    if (tud_hid_n_ready(ITF_MOUSE)) {
        if (tud_hid_n_get_protocol(ITF_MOUSE) == HID_PROTOCOL_BOOT) {
            if (!wheel) {
                uint8_t report[3] = {(uint8_t)(buttons & 7u), (uint8_t)dx, (uint8_t)dy};
                sent = tud_hid_n_report(ITF_MOUSE, 0, report, sizeof(report));
            }
        } else {
            sent = tud_hid_n_mouse_report(ITF_MOUSE, REPORT_ID_MOUSE, buttons, dx, dy, wheel, 0);
        }
    }
    hid_bridge_output_end(&bridge);
    return sent;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                              hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                          hid_report_type_t report_type, const uint8_t *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}

void tud_hid_report_complete_cb(uint8_t instance, const uint8_t *report, uint16_t length) {
    if (instance!=ITF_KEYBOARD) return;
    uint8_t empty[6]={0};
    unsigned offset=length==9 && report[0]==REPORT_ID_KEYBOARD ? 1u : 0u;
    bool valid=(length==8 || offset==1u) && report[offset+1]==0;
    hid_bridge_keyboard_complete(&bridge,valid?report[offset]:0,
                                  valid?report+offset+2:empty,(uint32_t)(time_us_64()/1000u),valid);
}

void tud_hid_report_failed_cb(uint8_t instance, hid_report_type_t report_type,
                             const uint8_t *report, uint16_t xferred_bytes) {
    (void)instance; (void)report_type; (void)report; (void)xferred_bytes;
    /* Dispatched by tud_task on Core 0, never by a network callback. */
    hid_bridge_cancel_reason(&bridge,HID_STOP_USB_FAILED,(uint32_t)(time_us_64()/1000u));
    ++engine.rejected;
    hid_engine_abort(&engine);
}

static void network_core(void) {
    wifi_service_run(&bridge, watchdog_boot);
}

int main(void) {
    stdio_init_all();
    watchdog_boot = watchdog_caused_reboot();
    watchdog_enable(8000, true);
    hid_bridge_init(&bridge); /* Fully initialized before publishing to Core 1. */
    hid_bridge_set_boot(&bridge, get_rand_32());
    hid_engine_init(&engine, (HidTransport){send_mouse, send_keyboard, NULL}, time_us_32());
    tusb_init();
    multicore_launch_core1_with_stack(network_core, network_core_stack, sizeof(network_core_stack));

    uint32_t reported_rejections = 0;
    while (true) {
        tud_task();
        uint32_t epoch = hid_bridge_epoch(&bridge);
        if (epoch != engine_epoch) {
            hid_engine_abort(&engine);
            engine_epoch = epoch;
        }
        if (!hid_engine_busy(&engine)) {
            HidCommand command;
            if (hid_bridge_take(&bridge, engine_epoch, &command)) {
                (void)hid_engine_submit(&engine, &command);
            }
        }
        uint32_t before_tick_rejections = engine.rejected;
        bool usb_available = tud_ready();
        hid_engine_tick(&engine, time_us_64(), usb_available);
        if (usb_available && engine.rejected != before_tick_rejections &&
            (engine.release_mouse || engine.release_keyboard)) {
            hid_bridge_cancel_reason(&bridge,HID_STOP_USB_STALL,(uint32_t)(time_us_64()/1000u));
        }
        uint8_t flags = (tud_mounted() ? 1u : 0u) |
                        (tud_hid_n_ready(ITF_KEYBOARD) ? 2u : 0u) |
                        (tud_hid_n_ready(ITF_MOUSE) ? 4u : 0u);
        uint64_t loop_now_us=time_us_64();
        hid_bridge_publish_at(&bridge, engine_epoch, flags, tud_ready(),
                           hid_engine_busy(&engine), engine.rejected - reported_rejections,loop_now_us);
        reported_rejections = engine.rejected;
        hid_bridge_lease_tick(&bridge, (uint32_t)(loop_now_us / 1000u), lease_keyboard, NULL);
        /* Only this core feeds the watchdog. Core 1 must complete its own main
         * loop; IRQ activity alone cannot hide a stalled networking task. */
        HidBridgeStatus status = hid_bridge_status(&bridge);
        uint32_t now_ms = (uint32_t)(loop_now_us / 1000u);
        if (status.network_heartbeat_ms && (uint32_t)(now_ms - status.network_heartbeat_ms) < 2000u) {
            watchdog_update();
        }
        tight_loop_contents();
    }
}
