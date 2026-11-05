#include "relay_state.h"

#include <string.h>

bool alarm_buf_push(alarm_buf_t *b, const uint8_t *pkt, size_t len, uint32_t now_ms)
{
    if (len == 0 || len > ALARM_PKT_MAX) {
        return false;
    }
    if (b->count == ALARM_BUF_CAP) {
        b->head = (b->head + 1) % ALARM_BUF_CAP;
        b->count--;
        b->overwritten++;
    }
    alarm_entry_t *e = &b->e[(b->head + b->count) % ALARM_BUF_CAP];
    e->len = (uint8_t)len;
    memcpy(e->data, pkt, len);
    e->at_ms = now_ms;
    b->count++;
    return true;
}

void alarm_buf_drop_oldest(alarm_buf_t *b)
{
    if (b->count) {
        b->head = (b->head + 1) % ALARM_BUF_CAP;
        b->count--;
    }
}

const alarm_entry_t *alarm_buf_oldest(alarm_buf_t *b, uint32_t now_ms, uint32_t max_age_ms)
{
    while (b->count) {
        const alarm_entry_t *e = &b->e[b->head];
        if ((uint32_t)(now_ms - e->at_ms) <= max_age_ms) {
            return e;
        }
        b->expired++;
        alarm_buf_drop_oldest(b);
    }
    return NULL;
}

void sensor_table_update(sensor_table_t *t, const wyze_event_t *ev, uint32_t now_ms)
{
    sensor_info_t *s = NULL;
    for (uint8_t i = 0; i < t->n; i++) {
        if (strcmp(t->s[i].mac, ev->mac) == 0) {
            s = &t->s[i];
            break;
        }
    }
    if (!s) {
        if (t->n < SENSOR_MAX) {
            s = &t->s[t->n++];
        } else {
            s = &t->s[0];
            for (uint8_t i = 1; i < t->n; i++) {
                if ((uint32_t)(now_ms - t->s[i].last_ms) > (uint32_t)(now_ms - s->last_ms)) {
                    s = &t->s[i];
                }
            }
        }
        memset(s, 0, sizeof(*s));
        memcpy(s->mac, ev->mac, sizeof(s->mac));
    }
    s->sensor_type = ev->sensor_type;
    s->last_event = ev->event_type;
    s->last_ms = now_ms;
    if (ev->has_signal) {
        s->has_signal = true;
        s->battery_raw = ev->battery_raw;
        s->rssi = ev->rssi;
    }
    if (ev->has_state) {
        s->has_state = true;
        s->state = ev->state;
    }
}
