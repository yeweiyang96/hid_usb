#include "wifi_watch.h"
#include <string.h>

void wifi_watch_init(WifiWatch *watch, uint64_t now_us) {
    memset(watch, 0, sizeof(*watch));
    watch->last_rx_us = now_us;
}

void wifi_watch_received(WifiWatch *watch, uint64_t now_us) {
    watch->last_rx_us = now_us;
    watch->missed = 0;
    watch->pending = false;
}

WifiWatchAction wifi_watch_tick(WifiWatch *w, uint64_t now,
                                bool initialized, bool up, bool gateway) {
    if (up && !w->up) {
        w->up = true;
        w->join_attempts = 0;
        wifi_watch_received(w, now);
        w->next_us = now + WIFI_PROBE_INTERVAL_US;
    } else if (!up && w->up) {
        w->up = false;
        w->pending = false;
        w->missed = 0;
        w->join_attempts = 0;
        w->next_us = now + WIFI_REJOIN_DELAY_US;
    }
    if (!up) {
        if (now < w->next_us) return WIFI_WATCH_NONE;
        w->next_us = now + WIFI_JOIN_TIMEOUT_US;
        w->reason = WIFI_RECOVERY_LINK_DOWN;
        if (!initialized || w->join_attempts) return WIFI_WATCH_RESET;
        w->join_attempts = 1;
        return WIFI_WATCH_REJOIN;
    }
    if (!gateway) return WIFI_WATCH_NONE;
    if (w->pending && now - w->probe_us >= WIFI_PROBE_TIMEOUT_US) {
        w->pending = false;
        ++w->missed;
        if (w->missed >= WIFI_MISSED_LIMIT) {
            w->reason = WIFI_RECOVERY_NO_RX;
            w->up = false;
            w->missed = 0;
            w->join_attempts = 1;
            w->next_us = now + WIFI_JOIN_TIMEOUT_US;
            return WIFI_WATCH_RESET;
        }
        w->next_us = now + WIFI_PROBE_RETRY_US;
    }
    if (!w->pending && now >= w->next_us) {
        w->pending = true;
        w->probe_us = now;
        w->next_us = now + WIFI_PROBE_INTERVAL_US;
        return WIFI_WATCH_PROBE;
    }
    return WIFI_WATCH_NONE;
}

bool wifi_watch_frame(const uint8_t *p, size_t n,
                      const uint8_t local[4], const uint8_t gateway[4]) {
    if (n < 14) return false;
    if (p[12] == 8 && p[13] == 0) {
        /* IPv4 unicast addressed to us; broadcasts cannot mask an outage. */
        return n >= 34 && (p[14] >> 4) == 4 && (p[14] & 15) >= 5 &&
               memcmp(p + 30, local, 4) == 0;
    }
    if (p[12] != 8 || p[13] != 6 || n < 42) return false;
    /* Ethernet/IPv4 ARP reply from our gateway, addressed to our IPv4. */
    static const uint8_t arp_header[8] = {0, 1, 8, 0, 6, 4, 0, 2};
    return memcmp(p + 14, arp_header, 8) == 0 &&
           memcmp(p + 28, gateway, 4) == 0 && memcmp(p + 38, local, 4) == 0;
}
