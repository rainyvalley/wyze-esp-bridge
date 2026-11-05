// Bounded state the relay keeps about the sensors it forwards: a ring of state-change reports
// held while the gateway is unreachable, and a table of the sensors seen. Plain data structures
// with no locking and no ESP-IDF dependencies (unit-tested on the host); callers serialise access.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wyze_proto.h"

// ---------------------------------------------------------------- held alarms

#define ALARM_BUF_CAP 32
#define ALARM_PKT_MAX 96  // real reports are ~35 bytes

typedef struct {
    uint8_t len;
    uint8_t data[ALARM_PKT_MAX];
    uint32_t at_ms;
} alarm_entry_t;

typedef struct {
    alarm_entry_t e[ALARM_BUF_CAP];
    uint8_t head;   // oldest entry
    uint8_t count;
    uint32_t overwritten;  // pushed out by newer ones because the ring was full
    uint32_t expired;      // too old to be worth replaying
} alarm_buf_t;

// Stores one packet; when full the oldest is dropped. Oversized packets are ignored (false).
bool alarm_buf_push(alarm_buf_t *b, const uint8_t *pkt, size_t len, uint32_t now_ms);

// The oldest packet younger than max_age_ms, or NULL when none is left (older ones are discarded
// and counted as expired). It stays queued until alarm_buf_drop_oldest(), so a failed send loses
// nothing. now_ms/at_ms are wrapping 32-bit milliseconds.
const alarm_entry_t *alarm_buf_oldest(alarm_buf_t *b, uint32_t now_ms, uint32_t max_age_ms);
void alarm_buf_drop_oldest(alarm_buf_t *b);

// ---------------------------------------------------------------- sensors seen

#define SENSOR_MAX 24

typedef struct {
    char mac[WYZE_MAC_LEN + 1];
    uint8_t sensor_type;
    uint8_t last_event;
    bool has_signal;
    uint8_t battery_raw;
    int8_t rssi;
    bool has_state;
    uint8_t state;
    uint32_t last_ms;
} sensor_info_t;

typedef struct {
    sensor_info_t s[SENSOR_MAX];
    uint8_t n;
} sensor_table_t;

// Records a report. A new sensor beyond SENSOR_MAX replaces the least recently heard one.
void sensor_table_update(sensor_table_t *t, const wyze_event_t *ev, uint32_t now_ms);
