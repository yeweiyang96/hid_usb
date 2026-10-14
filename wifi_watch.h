#ifndef WIFI_WATCH_H
#define WIFI_WATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIFI_PROBE_INTERVAL_US 15000000ull
#define WIFI_PROBE_TIMEOUT_US 3000000ull
#define WIFI_PROBE_RETRY_US 5000000ull
#define WIFI_REJOIN_DELAY_US 5000000ull
#define WIFI_JOIN_TIMEOUT_US 30000000ull
#define WIFI_MISSED_LIMIT 3u

typedef enum { WIFI_WATCH_NONE, WIFI_WATCH_PROBE, WIFI_WATCH_REJOIN,
               WIFI_WATCH_RESET } WifiWatchAction;
typedef enum { WIFI_RECOVERY_NONE, WIFI_RECOVERY_LINK_DOWN,
               WIFI_RECOVERY_NO_RX } WifiRecoveryReason;

typedef struct {
    bool up, pending;
    uint8_t missed, join_attempts;
    uint64_t last_rx_us, probe_us, next_us;
    WifiRecoveryReason reason;
} WifiWatch;

void wifi_watch_init(WifiWatch *watch, uint64_t now_us);
void wifi_watch_received(WifiWatch *watch, uint64_t now_us);
WifiWatchAction wifi_watch_tick(WifiWatch *watch, uint64_t now_us,
                                bool initialized, bool link_up, bool gateway);
/* Ethernet bytes in network order. Observe only; never consume/free a packet. */
bool wifi_watch_frame(const uint8_t *data, size_t size,
                      const uint8_t local_ip[4], const uint8_t gateway_ip[4]);

#endif
