#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/etharp.h"
#include "lwip/udp.h"

#include "wifi_service.h"
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

/* Core 1 main and its background callbacks share these under the lwIP lock.
 * The bridge is the only state visible to Core 0. No TinyUSB API is used here. */
static HidBridge *bridge;
static bool network_initialized, wifi_up, watchdog_boot;
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
    return original_input(p, netif);
}

static void link_changed(struct netif *netif) {
    if (!netif_is_link_up(netif) || !netif_is_up(netif)) {
        /* Independent of queue capacity; closes output and drops waiting work. */
        hid_bridge_network_at(bridge,false,false,(uint32_t)(time_us_64()/1000u));
    }
}

static void write_u32(uint8_t *out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (8u * i));
}

static void send_reply(struct udp_pcb *pcb,const ip_addr_t *addr,u16_t port,
                       const void *data,uint16_t size) {
    struct pbuf *reply=pbuf_alloc(PBUF_TRANSPORT,size,PBUF_RAM);
    if (!reply) { hid_bridge_note_send_error(bridge); return; }
    if (pbuf_take(reply,data,size)!=ERR_OK || udp_sendto(pcb,reply,addr,port)!=ERR_OK)
        hid_bridge_note_send_error(bridge);
    pbuf_free(reply);
}

static void send_health(struct udp_pcb *pcb, const ip_addr_t *addr, u16_t port,
                        uint32_t token, unsigned diagnostic_version) {
    uint8_t response[80];
    uint16_t size = diagnostic_version == 2 ? 80 : diagnostic_version == 1 ? 64 : HID_HEALTH_SIZE;
    HidBridgeStatus status = hid_bridge_status(bridge);
    uint8_t flags = status.flags | (LWIP_DHCP == 0 ? 32u : 0u) |
                    (watchdog_boot ? 64u : 0u) | 128u;
    hid_health_encode(response, token, flags, status.queued,
                      (uint32_t)(time_us_64() / 1000u), status.accepted, status.rejected);
    if (diagnostic_version) {
        memcpy(response, "PICD", 4);
        response[4] = (uint8_t)diagnostic_version;
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
        if (diagnostic_version == 2) {
            uint32_t now_ms = (uint32_t)(time_us_64() / 1000u);
            write_u32(response + 64, now_ms - status.network_heartbeat_ms);
            write_u32(response + 68, status.epoch);
            write_u32(response + 72, status.acknowledged_epoch);
            write_u32(response + 76, status.available ? 1u : 0u);
        }
    }
    send_reply(pcb,addr,port,response,size);
}

/* Runs under the SDK lwIP lock on Core 1, possibly in a low-priority IRQ.
 * Copy, validate and try-enqueue only. Never execute an input action here. */
static void udp_server_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                            const ip_addr_t *addr, u16_t port) {
    (void)arg;
    if (!p) return;
    ip_addr_t source;
    ip_addr_copy(source, *addr); /* addr may point into the packet. */
    uint8_t data[HID_MAX_PACKET_SIZE];
    uint16_t length = p->tot_len;
    if (!length || length > sizeof(data) || pbuf_copy_partial(p, data, length, 0) != length) {
        hid_bridge_reject(bridge);
        pbuf_free(p);
        return;
    }
    pbuf_free(p);
    if (data[0] == CMD_PING) {
        if (length == 5) {
            send_health(pcb, &source, port, hid_read_u32(data + 1), 0);
            return;
        }
        if ((length == 9 || length == 10) && memcmp(data + 5, "DIAG", 4) == 0 &&
            (length == 9 || data[9] == '2')) {
            send_health(pcb, &source, port, hid_read_u32(data + 1), length == 10 ? 2 : 1);
            return;
        }
    }
    if (length >= 4 && !memcmp(data, "ADRV", 4)) {
        if (length>=6 && data[4]==4) {
            uint8_t response[HID_MOTION_RESPONSE_SIZE];
            hid_bridge_motion_request(bridge,data,length,(uint32_t)(time_us_64()/1000u),response);
            send_reply(pcb,&source,port,response,sizeof(response));
        } else if (length>=6 && data[5]==LEASE_DIAGNOSTIC) {
            uint8_t diagnostic[HID_DIAGNOSTIC_SIZE];
            hid_bridge_diagnostic_request(bridge,data,length,(uint32_t)(time_us_64()/1000u),diagnostic);
            send_reply(pcb,&source,port,diagnostic,sizeof(diagnostic));
        } else {
            uint8_t response[HID_LEASE_RESPONSE_SIZE];
            hid_bridge_lease_request(bridge,data,length,(uint32_t)(time_us_64()/1000u),response);
            send_reply(pcb,&source,port,response,sizeof(response));
        }
        return;
    }
    HidCommand command;
    if (!hid_command_decode(data, length, &command)) {
        hid_bridge_reject(bridge);
        return;
    }
    if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP) {
        hid_bridge_network_at(bridge,false,false,(uint32_t)(time_us_64()/1000u));
    }
    if (command.type==CMD_RELEASE_ALL)
        hid_bridge_release_at(bridge,(uint32_t)(time_us_64()/1000u));
    else (void)hid_bridge_submit(bridge,&command);
}

static void network_start(void) {
    /* init/deinit on this core, without an already-held lwIP context lock. */
    network_initialized = cyw43_arch_init() == 0;
    if (!network_initialized) return;
    cyw43_arch_enable_sta_mode();
    cyw43_arch_lwip_begin();
    verify_power_mode();
    struct netif *netif = &cyw43_state.netif[CYW43_ITF_STA];
    original_input = netif->input;
    bool valid = original_input && netif_is_up(netif);
    if (valid) {
        netif->input = observed_input;
        netif_set_link_callback(netif, link_changed);
        netif_set_status_callback(netif, link_changed);
        server = udp_new_ip_type(IPADDR_TYPE_V4);
        if (server && udp_bind(server, IP_ANY_TYPE, UDP_PORT) == ERR_OK) {
            udp_recv(server, udp_server_recv, NULL);
        } else {
            if (server) udp_remove(server);
            server = NULL;
            valid = false;
        }
    }
    cyw43_arch_lwip_end();
    if (!valid) {
        cyw43_arch_deinit();
        network_initialized = false;
        original_input = NULL;
    }
}

static void network_reset(void) {
    /* Caller has already closed the bridge under lwIP -> bridge lock order.
     * Remove callbacks before deinit; never hold lwIP across arch_deinit. */
    wifi_up = false;
    if (network_initialized) {
        cyw43_arch_lwip_begin();
        if (server) udp_remove(server);
        server = NULL;
        cyw43_arch_lwip_end();
        cyw43_arch_deinit();
    }
    network_initialized = false;
    original_input = NULL;
    ++radio_resets; /* No network callbacks exist between deinit and init. */
    network_start();
    if (network_initialized) {
        cyw43_arch_lwip_begin();
        ++reconnect_count;
        join_result = cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
        cyw43_arch_lwip_end();
    }
}

static void wifi_task(uint64_t now_us) {
    static uint64_t next_check_us;
    if (now_us < next_check_us) return;
    next_check_us = now_us + 100000u;
    if (network_initialized) cyw43_arch_lwip_begin();
    int status = network_initialized ? cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) : CYW43_LINK_DOWN;
    bool up = status == CYW43_LINK_UP;
    if (!wifi_up && up) {
        verify_power_mode();
        (void)etharp_gratuitous(&cyw43_state.netif[CYW43_ITF_STA]);
    }
    wifi_up = up;
    hid_bridge_network_at(bridge,up,false,(uint32_t)(now_us/1000u));
    struct netif *netif = &cyw43_state.netif[CYW43_ITF_STA];
    bool gateway = network_initialized && !ip4_addr_isany_val(*netif_ip4_gw(netif));
    WifiWatchAction action = wifi_watch_tick(&wifi_watch, now_us, network_initialized, up, gateway);
    switch (action) {
    case WIFI_WATCH_PROBE:
        if (pm_result) verify_power_mode();
        (void)etharp_request(netif, netif_ip4_gw(netif));
        break;
    case WIFI_WATCH_REJOIN:
        hid_bridge_network_at(bridge,false,true,(uint32_t)(now_us/1000u));
        ++reconnect_count;
        (void)cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        join_result = cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
        break;
    case WIFI_WATCH_RESET:
        hid_bridge_network_at(bridge,false,true,(uint32_t)(now_us/1000u));
        break;
    default:
        break;
    }
    bool unlock = network_initialized;
    if (unlock) cyw43_arch_lwip_end();
    if (action == WIFI_WATCH_RESET) network_reset();
}

void wifi_service_run(HidBridge *shared_bridge, bool boot_from_watchdog) {
    bridge = shared_bridge;
    watchdog_boot = boot_from_watchdog;
    wifi_watch_init(&wifi_watch, time_us_64());
    network_start();
    while (true) {
        wifi_task(time_us_64());
        /* Only main-loop progress is credited, not background IRQ traffic. */
        hid_bridge_network_heartbeat_at(bridge,time_us_64());
        sleep_ms(1);
    }
}
