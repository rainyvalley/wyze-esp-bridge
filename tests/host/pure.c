// Host unit tests for the pure modules added with the web UI: the dongle packet scanner, the
// held-alarm ring and sensor table, and the log collapsing and snapshot formats. Unity, like
// tests/host/unit.c; no ESP-IDF.
#include "unity.h"

#include "log_dedup.h"
#include "log_snap.h"
#include "relay_state.h"
#include "wyze_proto.h"

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// The original assertions were plain `assert`s; keep their shape, let Unity report the line.
#define CHECK(cond) TEST_ASSERT_TRUE(cond)

static size_t seal(uint8_t *p, size_t body_len)  // p holds 55 AA type len cmd payload; appends checksum
{
    uint32_t s = 0;
    for (size_t i = 0; i < body_len; i++) s += p[i];
    p[body_len] = (s >> 8) & 0xFF;
    p[body_len + 1] = s & 0xFF;
    return body_len + 2;
}

// A 0x5319 report: event, mac, sensor type, [temp, battery, flags, marker, state, seq_hi, seq_lo, rssi]
static size_t report(uint8_t *p, uint8_t event, const char *mac, uint8_t stype, uint8_t battery, uint8_t state, uint8_t rssi)
{
    size_t n = 0;
    p[n++] = 0x55; p[n++] = 0xAA; p[n++] = 0x53; p[n++] = 0; p[n++] = 0x19;
    for (int i = 0; i < 7; i++) p[n++] = 0;
    p[n++] = 0x01;  // timestamp ms (8 bytes)
    p[n++] = event;
    memcpy(p + n, mac, 8); n += 8;
    p[n++] = stype;
    const uint8_t rem[] = {25, battery, 0, 1, state, 0, 7, rssi};
    memcpy(p + n, rem, sizeof(rem)); n += sizeof(rem);
    p[3] = (uint8_t)(n + 2 - 4);
    return seal(p, n);
}

typedef struct { int n; wyze_event_t last; } cap_t;
static void on_event(const wyze_event_t *ev, void *ctx) { cap_t *c = ctx; c->n++; c->last = *ev; }

static void test_packet_scan(void)
{
    uint8_t a[64], b[64], frame[160];
    cap_t cap = {0};
    bool all;

    size_t la = report(a, WYZE_EVENT_ALARM, "77A1B2C3", 0x01, 94, 1, 53);
    CHECK(wyze_scan_frame(a, la, on_event, &cap, &all) == 1 && all);
    CHECK(strcmp(cap.last.mac, "77A1B2C3") == 0 && cap.last.has_state && cap.last.state == 1);
    CHECK(cap.last.battery_raw == 94 && cap.last.rssi == -53 && cap.last.pkt == a && cap.last.pkt_len == la);

    // heartbeat: parsed but not replayable
    size_t lb = report(b, WYZE_EVENT_HEARTBEAT, "77A1B2C3", 0x01, 90, 0, 60);
    cap.n = 0;
    CHECK(wyze_scan_frame(b, lb, on_event, &cap, &all) == 1 && !all && !cap.last.has_state);

    // two packets back to back, both alarms
    memcpy(frame, a, la); memcpy(frame + la, a, la);
    CHECK(wyze_scan_frame(frame, 2 * la, NULL, NULL, &all) == 2 && all);
    // alarm + heartbeat: both seen, frame not alarm-only
    memcpy(frame + la, b, lb);
    CHECK(wyze_scan_frame(frame, la + lb, NULL, NULL, &all) == 2 && !all);

    // 7-byte async ACK in front of an alarm
    uint8_t ack[7] = {0x55, 0xAA, 0x53, 0x19, 0xFF};
    seal(ack, 5);
    memcpy(frame, ack, 7); memcpy(frame + 7, a, la);
    CHECK(wyze_scan_frame(frame, 7 + la, NULL, NULL, &all) == 1 && !all);

    // corrupted checksum / truncated / garbage / trailing junk
    memcpy(frame, a, la); frame[la - 1] ^= 0xFF;
    CHECK(wyze_scan_frame(frame, la, NULL, NULL, &all) == 0 && !all);
    CHECK(wyze_scan_frame(a, la - 1, NULL, NULL, &all) == 0 && !all);
    CHECK(wyze_scan_frame((const uint8_t *)"hello world", 11, NULL, NULL, &all) == 0 && !all);
    memcpy(frame, a, la); frame[la] = 0x00;
    CHECK(wyze_scan_frame(frame, la + 1, NULL, NULL, &all) == 1 && !all);
    CHECK(wyze_scan_frame(a, 0, NULL, NULL, &all) == 0 && !all);

    // leak alarms replay too
    size_t ll = report(a, WYZE_EVENT_LEAK, "77LEAK01", 0x03, 80, 1, 40);
    CHECK(wyze_scan_frame(a, ll, NULL, NULL, &all) == 1 && all);
}

static void test_alarm_ring(void)
{
    alarm_buf_t buf; memset(&buf, 0, sizeof(buf));
    uint8_t out[ALARM_PKT_MAX];
    const alarm_entry_t *e;
    for (int i = 0; i < ALARM_BUF_CAP + 5; i++) {
        uint8_t p[8] = {(uint8_t)i};
        CHECK(alarm_buf_push(&buf, p, sizeof(p), 1000 + i));
    }
    CHECK(buf.count == ALARM_BUF_CAP && buf.overwritten == 5);
    e = alarm_buf_oldest(&buf, 2000, 600000);
    CHECK(e && e->data[0] == 5 && e->len == 8);  // 0..4 were pushed out
    CHECK(alarm_buf_oldest(&buf, 2000, 600000) == e && buf.count == ALARM_BUF_CAP);  // peeking keeps it queued
    alarm_buf_drop_oldest(&buf);
    CHECK(alarm_buf_oldest(&buf, 2000, 600000)->data[0] == 6);
    CHECK(!alarm_buf_push(&buf, out, ALARM_PKT_MAX + 1, 0) && !alarm_buf_push(&buf, out, 0, 0));
    // expiry: everything left is older than the limit
    CHECK(alarm_buf_oldest(&buf, 1000 + 700000, 600000) == NULL);
    CHECK(buf.count == 0 && buf.expired == ALARM_BUF_CAP - 1);
    // wrap-safe ages across the 32-bit millisecond rollover
    uint8_t p1[4] = {1, 2, 3, 4};
    alarm_buf_push(&buf, p1, 4, 0xFFFFFF00u);
    e = alarm_buf_oldest(&buf, 0x100u, 1000);
    CHECK(e && e->len == 4);
}

static void test_sensors(void)
{
    uint8_t a[64], b[64];
    cap_t cap = {0};

    sensor_table_t t; memset(&t, 0, sizeof(t));
    size_t ll = report(a, WYZE_EVENT_LEAK, "77LEAK01", 0x03, 80, 1, 40);
    wyze_scan_frame(a, ll, on_event, &cap, NULL);
    sensor_table_update(&t, &cap.last, 10);
    size_t la = report(a, WYZE_EVENT_ALARM, "77A1B2C3", 0x01, 94, 1, 53);
    wyze_scan_frame(a, la, on_event, &cap, NULL);
    sensor_table_update(&t, &cap.last, 20);
    size_t lb = report(b, WYZE_EVENT_HEARTBEAT, "77A1B2C3", 0x01, 90, 0, 60);
    wyze_scan_frame(b, lb, on_event, &cap, NULL);
    sensor_table_update(&t, &cap.last, 30);
    CHECK(t.n == 2);
    CHECK(t.s[1].battery_raw == 90 && t.s[1].rssi == -60 && t.s[1].has_state && t.s[1].state == 1 && t.s[1].last_ms == 30);
    // overflow evicts the stalest
    for (int i = 0; i < SENSOR_MAX + 3; i++) {
        char mac[9]; snprintf(mac, sizeof(mac), "S%07d", i);
        size_t l = report(a, WYZE_EVENT_ALARM, mac, 0x02, 90, 0, 50);
        wyze_scan_frame(a, l, on_event, &cap, NULL);
        sensor_table_update(&t, &cap.last, 100 + i);
    }
    CHECK(t.n == SENSOR_MAX);
}

static void test_log_dedup(void)
{
    static log_dd_t d;
    memset(&d, 0, sizeof(d));
    char note[96];
    const char *m = "websocket_client: Reconnect after 5000 ms\n";
    uint32_t t0 = 1000;
    // the first three go out, then the loop is dropped
    for (int i = 0; i < LOG_DD_BURST; i++) CHECK(log_dd_pass(&d, m, t0 + i * 5000, note, sizeof(note)) && !note[0]);
    uint32_t t = t0 + LOG_DD_BURST * 5000;
    for (int i = 0; i < 1000 && !note[0]; i++, t += 5000) {
        CHECK(!log_dd_pass(&d, m, t, note, sizeof(note)));  // dropped, note or not
        CHECK(note[0] == '\0' || (uint32_t)(t - t0) >= LOG_DD_NOTE_MS);  // not before the interval
    }
    // a note appeared once the interval passed, with a sensible count and the message start
    CHECK(note[0] && strstr(note, "repeats of \"websocket_client: Reconnect after 5000 ms\""));
    int n = 0; sscanf(note, "%d repeats", &n);
    CHECK(n > 340 && n < 380);  // ~30 min / 5 s, minus the burst that was logged
    // a trailing newline does not make it a different message
    CHECK(!log_dd_pass(&d, "websocket_client: Reconnect after 5000 ms", t + 5000, note, sizeof(note)));
    // other messages are independent and unaffected
    CHECK(log_dd_pass(&d, "wyze-bridge: gateway connected\n", t, note, sizeof(note)));
    // the loop stops for longer than the window: logged again, with a note for what was dropped
    for (int i = 0; i < 5; i++) { t += 5000; log_dd_pass(&d, m, t, note, sizeof(note)); }  // keep it going
    t += LOG_DD_WINDOW_MS + 1;
    CHECK(log_dd_pass(&d, m, t, note, sizeof(note)));
    CHECK(note[0] && strstr(note, "repeats of"));
    // and it gets a fresh burst
    CHECK(log_dd_pass(&d, m, t + 1000, note, sizeof(note)) && !note[0]);
    CHECK(log_dd_pass(&d, m, t + 2000, note, sizeof(note)));
    CHECK(!log_dd_pass(&d, m, t + 3000, note, sizeof(note)));
    // slow repeats (gap > window each time) are never dropped
    memset(&d, 0, sizeof(d));
    for (int i = 0; i < 50; i++) CHECK(log_dd_pass(&d, "once a while\n", 1000 + i * (LOG_DD_WINDOW_MS + 1), note, sizeof(note)));
    // more distinct messages than slots: the oldest slot is reused, and says what it dropped
    memset(&d, 0, sizeof(d));
    for (int i = 0; i < LOG_DD_BURST + 5; i++) log_dd_pass(&d, "first message\n", 1000 + i, note, sizeof(note));
    bool said = false;
    for (int i = 0; i < LOG_DD_SLOTS + 1; i++) {
        char other[32]; snprintf(other, sizeof(other), "other %d\n", i);
        CHECK(log_dd_pass(&d, other, 2000 + i, note, sizeof(note)));
        if (note[0]) { said = true; CHECK(strstr(note, "first message")); }
    }
    CHECK(said);
    // an over-long message is cut in the note, never overflowing it
    memset(&d, 0, sizeof(d));
    char longmsg[400]; memset(longmsg, 'x', sizeof(longmsg) - 1); longmsg[sizeof(longmsg) - 1] = 0;
    for (int i = 0; i < 10; i++) log_dd_pass(&d, longmsg, 1000 + i, note, 16);
    CHECK(strlen(note) < 16);
}

static void test_log_snapshot(void)
{
    CHECK(logsnap_crc32(0, "123456789", 9) == 0xCBF43926u);                       // the standard check value
    CHECK(logsnap_crc32(logsnap_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u);  // chaining
    CHECK(logsnap_crc32(0, "", 0) == 0);
    logsnap_hdr_t h = {LOGSNAP_MAGIC, 7, 100, 0};
    CHECK(logsnap_hdr_plausible(&h, 4096));
    h.len = 0; CHECK(!logsnap_hdr_plausible(&h, 4096));
    h.len = 5000; CHECK(!logsnap_hdr_plausible(&h, 4096));
    h.len = 10; h.magic = 0xFFFFFFFFu; CHECK(!logsnap_hdr_plausible(&h, 4096));  // erased flash
    CHECK(logsnap_newer(5, 4) && !logsnap_newer(4, 5) && !logsnap_newer(4, 4));
    CHECK(logsnap_newer(0, 0xFFFFFFFFu) && !logsnap_newer(0xFFFFFFFFu, 0));       // wraps
    // layout: everything fits
    logsnap_layout_t l = logsnap_layout(100, 16, 200, 1000);
    CHECK(l.prev == 100 && l.marker == 16 && l.cur == 200 && !l.cut);
    // the old run is trimmed first
    l = logsnap_layout(900, 16, 200, 1000);
    CHECK(l.cur == 200 && l.marker == 16 && l.prev == 784 && l.cut);
    // a run bigger than the slot keeps only its newest bytes and drops the rest
    l = logsnap_layout(900, 16, 5000, 1000);
    CHECK(l.cur == 1000 && l.marker == 0 && l.prev == 0 && l.cut);
    // exactly full
    l = logsnap_layout(0, 0, 1000, 1000);
    CHECK(l.cur == 1000 && !l.cut);
    // a cut text starts at a line boundary
    const char *txt = "rtial line\nI (1) full line\n";
    CHECK(logsnap_line_start(txt, strlen(txt)) == 11);
    CHECK(logsnap_line_start("no newline at all", 17) == 0);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_packet_scan);
    RUN_TEST(test_alarm_ring);
    RUN_TEST(test_sensors);
    RUN_TEST(test_log_dedup);
    RUN_TEST(test_log_snapshot);
    return UNITY_END();
}
