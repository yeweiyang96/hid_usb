#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/udp.h"
#include "tusb.h"

#include "hid_engine.h"
#include "usb_descriptors.h"

#ifdef HID_HAS_WIFI_CONFIG
#include "wifi_config.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID "CHANGE_ME"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "CHANGE_ME"
#endif
#ifndef UDP_PORT
#define UDP_PORT 8888
#endif

enum { ITF_KEYBOARD = 0, ITF_MOUSE = 1 };
static HidEngine engine;
static bool wifi_up;

static bool send_keyboard(void *context, uint8_t modifier, const uint8_t keys[6]) {
    (void)context;
    if (!tud_hid_n_ready(ITF_KEYBOARD)) return false;
    uint8_t id = tud_hid_n_get_protocol(ITF_KEYBOARD) == HID_PROTOCOL_BOOT ? 0 : REPORT_ID_KEYBOARD;
    return tud_hid_n_keyboard_report(ITF_KEYBOARD, id, modifier, keys);
}

static bool send_mouse(void *context, uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel) {
    (void)context;
    if (!tud_hid_n_ready(ITF_MOUSE)) return false;
    if (tud_hid_n_get_protocol(ITF_MOUSE) == HID_PROTOCOL_BOOT) {
        /* Boot mouse has no wheel; reject rather than claim scroll succeeded. */
        if (wheel) return false;
        uint8_t report[3] = {(uint8_t)(buttons & 7u), (uint8_t)dx, (uint8_t)dy};
        return tud_hid_n_report(ITF_MOUSE, 0, report, sizeof(report));
    }
    return tud_hid_n_mouse_report(ITF_MOUSE, REPORT_ID_MOUSE, buttons, dx, dy, wheel, 0);
}

static void send_health(struct udp_pcb *pcb, const ip_addr_t *addr, u16_t port, uint32_t token) {
    uint8_t response[HID_HEALTH_SIZE];
    uint8_t flags = (tud_mounted() ? 1u : 0u) |
                    (tud_hid_n_ready(ITF_KEYBOARD) ? 2u : 0u) |
                    (tud_hid_n_ready(ITF_MOUSE) ? 4u : 0u) |
                    (hid_engine_busy(&engine) ? 8u : 0u) |
                    (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP ? 16u : 0u) |
                    (LWIP_DHCP == 0 ? 32u : 0u);
    hid_health_encode(response, token, flags, engine.count,
                      (uint32_t)(time_us_64() / 1000u), engine.accepted, engine.rejected);
    struct pbuf *reply = pbuf_alloc(PBUF_TRANSPORT, sizeof(response), PBUF_RAM);
    if (!reply) return;
    if (pbuf_take(reply, response, sizeof(response)) == ERR_OK) {
        (void)udp_sendto(pcb, reply, addr, port);
    }
    pbuf_free(reply);
}

/* In poll mode this callback runs on the same core as the USB task.
 * Copy the whole datagram (which may be a pbuf chain), validate and return.
 * A ping bypasses the action queue and never submits a HID report. */
static void udp_server_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                            const ip_addr_t *addr, u16_t port) {
    (void)arg;
    if (!p) return;
    /* lwIP permits addr to point into p, so retain it before freeing p. */
    ip_addr_t source;
    ip_addr_copy(source, *addr);
    uint8_t data[HID_MAX_PACKET_SIZE];
    uint16_t length = p->tot_len;
    if (!length || length > sizeof(data) || pbuf_copy_partial(p, data, length, 0) != length) {
        ++engine.rejected;
        pbuf_free(p);
        return;
    }
    pbuf_free(p);
    if (data[0] == CMD_PING && length == 5) {
        send_health(pcb, &source, port, hid_read_u32(data + 1));
        return;
    }
    HidCommand command;
    if (!hid_command_decode(data, length, &command) || !tud_ready() ||
        cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP) {
        ++engine.rejected;
        return;
    }
    (void)hid_engine_submit(&engine, &command);
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

void tud_hid_report_failed_cb(uint8_t instance, hid_report_type_t report_type,
                             const uint8_t *report, uint16_t xferred_bytes) {
    (void)instance; (void)report_type; (void)report; (void)xferred_bytes;
    /* The host may have observed part of an action: abort, never replay it. */
    ++engine.rejected;
    hid_engine_abort(&engine);
}

static void wifi_task(uint64_t now_us) {
    static uint64_t next_check_us, next_join_us;
    if (now_us < next_check_us) return;
    next_check_us = now_us + 100000u;
    int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    bool up = status == CYW43_LINK_UP;
    if (wifi_up && !up) hid_engine_abort(&engine);
    wifi_up = up;
    if (up) {
        next_join_us = now_us + 5000000u;
    } else if (now_us >= next_join_us) {
        /* Leave/rejoin also recovers from an association attempt that stalled.
         * Static IPv4 settings are reapplied by the CYW43 driver on STA init. */
        (void)cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        (void)cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
        next_join_us = now_us + 30000000u;
    }
}

int main(void) {
    stdio_init_all();
    hid_engine_init(&engine, (HidTransport){send_mouse, send_keyboard, NULL}, time_us_32());
    tusb_init();

    bool network_initialized = cyw43_arch_init() == 0;
    struct udp_pcb *pcb = NULL;
    if (network_initialized) {
        cyw43_arch_enable_sta_mode();
        (void)cyw43_wifi_pm(&cyw43_state, CYW43_NO_POWERSAVE_MODE);
        /* RAW lwIP and TinyUSB are owned exclusively by this application loop. */
        pcb = udp_new_ip_type(IPADDR_TYPE_V4);
        if (pcb && udp_bind(pcb, IP_ANY_TYPE, UDP_PORT) == ERR_OK) {
            udp_recv(pcb, udp_server_recv, NULL);
        } else {
            if (pcb) udp_remove(pcb);
            pcb = NULL;
        }
    }

    while (true) {
        tud_task();
        if (network_initialized) {
            cyw43_arch_poll();
            wifi_task(time_us_64());
        }
        tud_task();
        hid_engine_tick(&engine, time_us_64(), tud_ready());
        tight_loop_contents();
    }
}
