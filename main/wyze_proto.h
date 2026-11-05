// Minimal reader for the Wyze dongle -> host packets this bridge relays. The bridge stays a
// transparent relay; this only looks at sensor reports (0x5319) so it can hold state-change
// events while the gateway is unreachable and show per-sensor signal/battery on the web page.
// No ESP-IDF dependencies: unit-tested on the host (tests/).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WYZE_MAC_LEN 8

#define WYZE_EVENT_HEARTBEAT 0xA1
#define WYZE_EVENT_ALARM 0xA2
#define WYZE_EVENT_ALARM_DATA 0xAB
#define WYZE_EVENT_CLIMATE 0xE8
#define WYZE_EVENT_LEAK 0xEA

typedef struct {
    uint8_t event_type;
    uint8_t sensor_type;
    char mac[WYZE_MAC_LEN + 1];  // ASCII, NUL-terminated ("?" characters if the bytes were not text)
    bool has_signal;             // battery_raw / rssi are valid
    uint8_t battery_raw;         // volts ~= battery_raw / 32
    int8_t rssi;                 // dBm
    bool has_state;
    uint8_t state;  // 0 = inactive, 1 = active (WYZE_EVENT_ALARM)
    const uint8_t *pkt;  // the whole packet inside the scanned buffer, for replaying it verbatim
    size_t pkt_len;
} wyze_event_t;

typedef void (*wyze_event_cb)(const wyze_event_t *ev, void *ctx);

// Events worth holding back while the gateway is down: sensor state changes. Heartbeats and
// climate readings repeat on their own, so replaying them later would only be stale noise.
bool wyze_event_replayable(uint8_t event_type);

// Walks the packets in one dongle -> gateway frame (a frame can hold several back to back) and
// calls cb for every valid 0x5319 sensor report. Checksums are verified; scanning stops at the
// first malformed packet. Returns the number of reports found. *all_replayable (optional) is set
// true only when the frame consists solely of replayable reports and nothing else.
size_t wyze_scan_frame(const uint8_t *buf, size_t len, wyze_event_cb cb, void *ctx, bool *all_replayable);

const char *wyze_sensor_type_name(uint8_t sensor_type);
