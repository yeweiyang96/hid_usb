#include "wifi_watch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void test_reconnect(void) {
    WifiWatch w;
    wifi_watch_init(&w, 0);
    CHECK(wifi_watch_tick(&w, 0, true, false, true) == WIFI_WATCH_REJOIN);
    CHECK(wifi_watch_tick(&w, WIFI_JOIN_TIMEOUT_US - 1, true, false, true) == WIFI_WATCH_NONE);
    CHECK(wifi_watch_tick(&w, WIFI_JOIN_TIMEOUT_US, true, false, true) == WIFI_WATCH_RESET);
    /* No driver / failed initialization is retried with a bounded interval. */
    CHECK(wifi_watch_tick(&w, WIFI_JOIN_TIMEOUT_US + 1, false, false, true) == WIFI_WATCH_NONE);
    CHECK(wifi_watch_tick(&w, 2 * WIFI_JOIN_TIMEOUT_US, false, false, true) == WIFI_WATCH_RESET);
    uint64_t now = 2 * WIFI_JOIN_TIMEOUT_US + 1;
    CHECK(wifi_watch_tick(&w, now, true, true, true) == WIFI_WATCH_NONE);
    CHECK(w.up && !w.join_attempts);
    CHECK(wifi_watch_tick(&w, now + 1, true, false, true) == WIFI_WATCH_NONE);
    CHECK(wifi_watch_tick(&w, now + WIFI_REJOIN_DELAY_US, true, false, true) == WIFI_WATCH_NONE);
    CHECK(wifi_watch_tick(&w, now + 1 + WIFI_REJOIN_DELAY_US, true, false, true) == WIFI_WATCH_REJOIN);
    puts("PASS link loss, stalled join and initialization retry");
}

static void test_alive(void) {
    WifiWatch w;
    wifi_watch_init(&w, 0);
    CHECK(wifi_watch_tick(&w, 1, true, true, true) == WIFI_WATCH_NONE);
    CHECK(wifi_watch_tick(&w, WIFI_PROBE_INTERVAL_US, true, true, true) == WIFI_WATCH_NONE);
    uint64_t now = 1 + WIFI_PROBE_INTERVAL_US;
    for (unsigned i = 0; i < 1000; ++i) {
        CHECK(wifi_watch_tick(&w, now, true, true, true) == WIFI_WATCH_PROBE);
        CHECK(w.pending);
        wifi_watch_received(&w, now + 1000);
        CHECK(!w.pending && !w.missed);
        CHECK(wifi_watch_tick(&w, now + WIFI_PROBE_TIMEOUT_US, true, true, true) == WIFI_WATCH_NONE);
        now += WIFI_PROBE_INTERVAL_US;
    }
    /* Gateway absent: keep normal link monitoring without false RX failures. */
    CHECK(wifi_watch_tick(&w, now, true, true, false) == WIFI_WATCH_NONE);
    puts("PASS long idle with gateway replies and no-gateway mode");
}

static void test_stale_link(void) {
    WifiWatch w;
    wifi_watch_init(&w, 0);
    (void)wifi_watch_tick(&w, 0, true, true, true);
    uint64_t now = WIFI_PROBE_INTERVAL_US;
    for (unsigned i = 1; i <= WIFI_MISSED_LIMIT; ++i) {
        CHECK(wifi_watch_tick(&w, now, true, true, true) == WIFI_WATCH_PROBE);
        CHECK(wifi_watch_tick(&w, now + WIFI_PROBE_TIMEOUT_US - 1, true, true, true) == WIFI_WATCH_NONE);
        WifiWatchAction result = wifi_watch_tick(&w, now + WIFI_PROBE_TIMEOUT_US, true, true, true);
        CHECK(result == (i == WIFI_MISSED_LIMIT ? WIFI_WATCH_RESET : WIFI_WATCH_NONE));
        now += WIFI_PROBE_TIMEOUT_US + WIFI_PROBE_RETRY_US;
    }
    CHECK(w.reason == WIFI_RECOVERY_NO_RX && !w.up);
    CHECK(wifi_watch_tick(&w, now, true, false, true) == WIFI_WATCH_NONE);
    CHECK(wifi_watch_tick(&w, now + WIFI_JOIN_TIMEOUT_US, true, false, true) == WIFI_WATCH_RESET);
    (void)wifi_watch_tick(&w, now + WIFI_JOIN_TIMEOUT_US + 1, true, true, true);
    CHECK(w.up && !w.missed);
    puts("PASS stale UP flag causes radio reset and recovery");
}

static void test_packet_loss(void) {
    WifiWatch w;
    wifi_watch_init(&w, 0);
    (void)wifi_watch_tick(&w, 0, true, true, true);
    (void)wifi_watch_tick(&w, WIFI_PROBE_INTERVAL_US, true, true, true);
    (void)wifi_watch_tick(&w, WIFI_PROBE_INTERVAL_US + WIFI_PROBE_TIMEOUT_US, true, true, true);
    CHECK(w.missed == 1);
    wifi_watch_received(&w, WIFI_PROBE_INTERVAL_US + WIFI_PROBE_TIMEOUT_US + 1);
    CHECK(!w.missed && !w.pending);
    /* A long hold/input action changes no RX policy and cannot suppress probes. */
    CHECK(wifi_watch_tick(&w, WIFI_PROBE_INTERVAL_US + WIFI_PROBE_TIMEOUT_US + WIFI_PROBE_RETRY_US,
                          true, true, true) == WIFI_WATCH_PROBE);
    puts("PASS isolated loss and passive receive prevent unnecessary reset");
}

static void test_frames(void) {
    const uint8_t local[4] = {192, 168, 11, 7}, gateway[4] = {192, 168, 11, 1};
    uint8_t p[42] = {0};
    p[12] = 8; p[13] = 6;
    const uint8_t arp[8] = {0, 1, 8, 0, 6, 4, 0, 2};
    memcpy(p + 14, arp, 8);
    memcpy(p + 28, gateway, 4); memcpy(p + 38, local, 4);
    CHECK(wifi_watch_frame(p, sizeof(p), local, gateway));
    for (size_t n = 0; n < sizeof(p); ++n) CHECK(!wifi_watch_frame(p, n, local, gateway));
    p[21] = 1; CHECK(!wifi_watch_frame(p, sizeof(p), local, gateway)); p[21] = 2;
    p[31] = 2; CHECK(!wifi_watch_frame(p, sizeof(p), local, gateway)); p[31] = 1;
    p[41] = 8; CHECK(!wifi_watch_frame(p, sizeof(p), local, gateway));
    memset(p, 0, sizeof(p)); p[12] = 8; p[14] = 0x45; memcpy(p + 30, local, 4);
    CHECK(wifi_watch_frame(p, sizeof(p), local, gateway));
    CHECK(!wifi_watch_frame(p, 33, local, gateway));
    p[33] = 255; CHECK(!wifi_watch_frame(p, sizeof(p), local, gateway)); p[33] = 7;
    p[14] = 0x44; CHECK(!wifi_watch_frame(p, sizeof(p), local, gateway));
    p[14] = 0x65; CHECK(!wifi_watch_frame(p, sizeof(p), local, gateway));
    puts("PASS ARP and unicast RX validation with truncated/unrelated frames");
}

int main(void) {
    test_reconnect(); test_alive(); test_stale_link(); test_packet_loss(); test_frames();
    puts("All Wi-Fi watch tests passed.");
    return 0;
}
