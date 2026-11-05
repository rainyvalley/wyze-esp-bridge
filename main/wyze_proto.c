#include "wyze_proto.h"

#include <string.h>

#define MAGIC0 0x55
#define MAGIC1 0xAA
#define TYPE_ASYNC 0x53
#define CMD_SENSOR_REPORT 0x19  // 0x5319
#define CMD_ASYNC_ACK 0xFF      // 0x53FF: fixed 7-byte packet whose byte 3 is not a length
#define REPORT_HDR 18           // timestamp(8) event(1) mac(8) sensor type(1)

static uint16_t checksum(const uint8_t *p, size_t n)
{
    uint32_t sum = 0;
    while (n--) {
        sum += *p++;
    }
    return (uint16_t)(sum & 0xFFFF);
}

bool wyze_event_replayable(uint8_t event_type)
{
    return event_type == WYZE_EVENT_ALARM || event_type == WYZE_EVENT_LEAK;
}

const char *wyze_sensor_type_name(uint8_t t)
{
    switch (t) {
    case 0x01: return "contact";
    case 0x02: return "motion";
    case 0x03: return "leak";
    case 0x07: return "climate";
    case 0x0C: return "chime";
    case 0x0E: return "contact v2";
    case 0x0F: return "motion v2";
    default: return "unknown";
    }
}

// payload = bytes after the 5-byte header, before the 2-byte checksum.
static void parse_report(const uint8_t *pkt, size_t pkt_len, wyze_event_t *ev)
{
    const uint8_t *pl = pkt + 5;
    const size_t pl_len = pkt_len - 7;
    memset(ev, 0, sizeof(*ev));
    ev->pkt = pkt;
    ev->pkt_len = pkt_len;
    ev->event_type = pl[8];
    ev->sensor_type = pl[17];
    for (int i = 0; i < WYZE_MAC_LEN; i++) {
        const uint8_t c = pl[9 + i];
        ev->mac[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    ev->mac[WYZE_MAC_LEN] = '\0';
    const uint8_t *r = pl + REPORT_HDR;
    const size_t rn = pl_len - REPORT_HDR;
    switch (ev->event_type) {
    case WYZE_EVENT_HEARTBEAT:
    case WYZE_EVENT_ALARM:
        if (rn >= 8) {
            ev->has_signal = true;
            ev->battery_raw = r[1];
            ev->rssi = (int8_t)(-(int)(int8_t)r[7]);
            if (ev->event_type == WYZE_EVENT_ALARM) {
                ev->has_state = true;
                ev->state = r[4];
            }
        }
        break;
    case WYZE_EVENT_CLIMATE:
        if (rn >= 10) {
            ev->has_signal = true;
            ev->battery_raw = r[1];
            ev->rssi = (int8_t)(-(int)(int8_t)r[9]);
        }
        break;
    default:
        break;
    }
}

size_t wyze_scan_frame(const uint8_t *buf, size_t len, wyze_event_cb cb, void *ctx, bool *all_replayable)
{
    size_t off = 0, found = 0;
    bool all = len > 0;
    while (off < len) {
        const uint8_t *p = buf + off;
        const size_t rem = len - off;
        if (rem < 7 || p[0] != MAGIC0 || p[1] != MAGIC1) {
            all = false;
            break;
        }
        const bool ack = p[2] == TYPE_ASYNC && p[4] == CMD_ASYNC_ACK;
        const size_t plen = ack ? 7 : (size_t)p[3] + 4;
        if (plen < 7 || plen > rem ||
            checksum(p, plen - 2) != (uint16_t)((p[plen - 2] << 8) | p[plen - 1])) {
            all = false;
            break;
        }
        bool replayable = false;
        if (p[2] == TYPE_ASYNC && p[4] == CMD_SENSOR_REPORT && plen >= 5 + REPORT_HDR + 2) {
            wyze_event_t ev;
            parse_report(p, plen, &ev);
            found++;
            replayable = wyze_event_replayable(ev.event_type);
            if (cb) {
                cb(&ev, ctx);
            }
        }
        all = all && replayable;
        off += plen;
    }
    if (all_replayable) {
        *all_replayable = all && off == len;
    }
    return found;
}
