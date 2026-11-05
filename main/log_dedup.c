#include "log_dedup.h"

#include <stdio.h>
#include <string.h>

static uint32_t hash_msg(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s && *s != '\n'; s++) {
        h ^= (uint8_t)*s;
        h *= 16777619u;
    }
    return h;
}

static void make_note(const log_dd_slot_t *sl, char *note, size_t size)
{
    snprintf(note, size, "%u repeats of \"%s\"", (unsigned)sl->suppressed, sl->text);
}

bool log_dd_pass(log_dd_t *d, const char *msg, uint32_t now_ms, char *note, size_t note_size)
{
    if (note_size) {
        note[0] = '\0';
    }
    const uint32_t h = hash_msg(msg);
    log_dd_slot_t *sl = NULL;
    for (int i = 0; i < LOG_DD_SLOTS; i++) {
        if (d->s[i].used && d->s[i].hash == h) {
            sl = &d->s[i];
            break;
        }
    }
    if (!sl) {
        sl = &d->s[d->next];
        d->next = (uint8_t)((d->next + 1) % LOG_DD_SLOTS);
        if (sl->used && sl->suppressed && note_size) {
            make_note(sl, note, note_size);  // the message loses its slot: say what was dropped
        }
        memset(sl, 0, sizeof(*sl));
        sl->used = true;
        sl->hash = h;
        sl->burst = 1;
        sl->last_ms = sl->note_ms = now_ms;
        size_t n = 0;
        for (; msg[n] && msg[n] != '\n' && n < sizeof(sl->text) - 1; n++) {
            sl->text[n] = msg[n];
        }
        sl->text[n] = '\0';
        return true;
    }
    if ((uint32_t)(now_ms - sl->last_ms) > LOG_DD_WINDOW_MS) {  // it stopped for a while: a new burst
        if (sl->suppressed && note_size) {
            make_note(sl, note, note_size);
        }
        sl->burst = 1;
        sl->suppressed = 0;
        sl->last_ms = sl->note_ms = now_ms;
        return true;
    }
    sl->last_ms = now_ms;
    if (sl->burst < LOG_DD_BURST) {
        sl->burst++;
        return true;
    }
    sl->suppressed++;
    if ((uint32_t)(now_ms - sl->note_ms) >= LOG_DD_NOTE_MS && note_size) {
        make_note(sl, note, note_size);
        sl->suppressed = 0;
        sl->note_ms = now_ms;
    }
    return false;
}
