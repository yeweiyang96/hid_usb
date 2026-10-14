#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"
#include "lwip/etharp.h"
#include "lwip/udp.h"
#include "tusb.h"

#include "hid_engine.h"
#include "usb_descriptors.h"
#include "wifi_watch.h"

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
static bool network_initialized, watchdog_boot;
static struct udp_pcb *server;
static netif_input_fn original_input;
static WifiWatch wifi_watch;
static uint32_t reconnect_count, radio_resets, pm_value = UINT32_MAX;
static int pm_result = -1, join_result;

static void verify_power_mode(void) {
    pm_result = cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
    if (!pm_result) pm_result = cyw43_wifi_get_pm(&cyw43_state, &pm_value);
    if (!pm_result && (pm_value & 15u) != CYW43_NO_POWERSAVE_MODE) pm_result = -2;
}

static err_t observed_input(struct pbuf *p, struct netif *netif) {
    uint8_t frame[42];
    uint16_t size = p->tot_len < sizeof(frame) ? p->tot_len : sizeof(frame);
    if (pbuf_copy_partial(p, frame, size, 0) == size &&
        wifi_watch_frame(frame, size, (const uint8_t *)&netif_ip4_addr(netif)->addr,
                         (const uint8_t *)&netif_ip4_gw(netif)->addr)) {
        wifi_watch_received(&wifi_watch, time_us_64());
    }
    /* Forward the original pbuf unchanged; lwIP retains ownership rules. */
    return original_input(p, netif);
}

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

static void write_u32(uint8_t *out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (8u * i));
}

static void send_health(struct udp_pcb *pcb, const ip_addr_t *addr, u16_t port,
                        uint32_t token, bool diagnostics) {
    uint8_t response[64];
    uint16_t size = diagnostics ? sizeof(response) : HID_HEALTH_SIZE;
    uint8_t flags = (tud_mounted() ? 1u : 0u) |
                    (tud_hid_n_ready(ITF_KEYBOARD) ? 2u : 0u) |
                    (tud_hid_n_ready(ITF_MOUSE) ? 4u : 0u) |
                    (hid_engine_busy(&engine) ? 8u : 0u) |
                    (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP ? 16u : 0u) |
                    (LWIP_DHCP == 0 ? 32u : 0u) |
                    (watchdog_boot ? 64u : 0u);
    hid_health_encode(response, token, flags, engine.count,
                      (uint32_t)(time_us_64() / 1000u), engine.accepted, engine.rejected);
    if (diagnostics) {
        memcpy(response, "PICD", 4);
        response[4] = 1; /* Independent diagnostic format version. */
        write_u32(response + 24, reconnect_count);
        write_u32(response + 28, radio_resets);
        write_u32(response + 32, (uint32_t)((time_us_64() - wifi_watch.last_rx_us) / 1000u));
        write_u32(response + 36, pm_value);
        write_u32(response + 40, (uint32_t)pm_result);
        write_u32(response + 44, (uint32_t)join_result);
        write_u32(response + 48, (uint32_t)cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA));
        write_u32(response + 52, watchdog_boot ? 1u : 0u);
        write_u32(response + 56, wifi_watch.missed);
        write_u32(response + 60, (uint32_t)wifi_watch.reason);
    }
    struct pbuf *reply = pbuf_alloc(PBUF_TRANSPORT, size, PBUF_RAM);
    if (!reply) return;
    if (pbuf_take(reply, response, size) == ERR_OK) {
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
        send_health(pcb, &source, port, hid_read_u32(data + 1), false);
        return;
    }
    if (data[0] == CMD_PING && length == 9 && memcmp(data + 5, "DIAG", 4) == 0) {
        send_health(pcb, &source, port, hid_read_u32(data + 1), true);
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

static void network_start(void) {
    network_initialized = cyw43_arch_init() == 0;
    if (!network_initialized) return;
    cyw43_arch_enable_sta_mode();
    verify_power_mode();
    struct netif *netif = &cyw43_state.netif[CYW43_ITF_STA];
    original_input = netif->input;
    if (!original_input || !netif_is_up(netif)) {
        cyw43_arch_deinit();
        network_initialized = false;
        return;
    }
    netif->input = observed_input;
    server = udp_new_ip_type(IPADDR_TYPE_V4);
    if (server && udp_bind(server, IP_ANY_TYPE, UDP_PORT) == ERR_OK) {
        udp_recv(server, udp_server_recv, NULL);
    } else {
        if (server) udp_remove(server);
        server = NULL;
        cyw43_arch_deinit();
        network_initialized = false;
        original_input = NULL;
    }
}

static void network_reset(void) {
    /* Called only from the main loop, after cyw43_arch_poll has returned. */
    hid_engine_abort(&engine);
    hid_engine_tick(&engine, time_us_64(), tud_ready());
    tud_task();
    wifi_up = false;
    if (server) udp_remove(server);
    server = NULL;
    if (network_initialized) cyw43_arch_deinit();
    network_initialized = false;
    original_input = NULL;
    ++radio_resets;
    network_start();
    if (network_initialized) {
        ++reconnect_count;
        join_result = cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
    }
}

static void wifi_task(uint64_t now_us) {
    static uint64_t next_check_us;
    if (now_us < next_check_us) return;
    next_check_us = now_us + 100000u;
    int status = network_initialized ? cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) : CYW43_LINK_DOWN;
    bool up = status == CYW43_LINK_UP;
    if (wifi_up && !up) hid_engine_abort(&engine);
    if (!wifi_up && up) {
        verify_power_mode();
        (void)etharp_gratuitous(&cyw43_state.netif[CYW43_ITF_STA]);
    }
    wifi_up = up;
    struct netif *netif = &cyw43_state.netif[CYW43_ITF_STA];
    bool gateway = network_initialized && !ip4_addr_isany_val(*netif_ip4_gw(netif));
    switch (wifi_watch_tick(&wifi_watch, now_us, network_initialized, up, gateway)) {
    case WIFI_WATCH_PROBE:
        /* Fresh ARP replies test the RX path; a cached link flag cannot do so.
         * ARP also works on routers that ignore ICMP ping. */
        if (pm_result) verify_power_mode();
        (void)etharp_request(netif, netif_ip4_gw(netif));
        break;
    case WIFI_WATCH_REJOIN:
        ++reconnect_count;
        (void)cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        join_result = cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
        break;
    case WIFI_WATCH_RESET:
        network_reset();
        break;
    default:
        break;
    }
}

int main(void) {
    stdio_init_all();
    watchdog_boot = watchdog_caused_reboot();
    /* Feed only after the USB/network application loop has made progress. */
    watchdog_enable(8000, true);
    hid_engine_init(&engine, (HidTransport){send_mouse, send_keyboard, NULL}, time_us_32());
    wifi_watch_init(&wifi_watch, time_us_64());
    tusb_init();
    network_start();

    while (true) {
        tud_task();
        if (network_initialized) {
            cyw43_arch_poll();
        }
        wifi_task(time_us_64());
        tud_task();
        hid_engine_tick(&engine, time_us_64(), tud_ready());
        watchdog_update();
        tight_loop_contents();
    }
}
