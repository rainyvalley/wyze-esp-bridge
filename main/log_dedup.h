// Collapses log lines that repeat on a loop (a gateway that is down is retried every 5 s and each
// attempt writes the same ~7 lines). The first few repeats are logged normally, later ones are
// dropped, and one note per message says how many were dropped. Without it such a loop pushes
// everything useful out of the log buffer within minutes. No ESP-IDF dependencies: unit-tested.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOG_DD_SLOTS 24                      // distinct messages tracked at once
#define LOG_DD_BURST 3                       // lines logged in full before dropping starts
#define LOG_DD_WINDOW_MS 60000u              // a gap this long ends a burst: the next line is logged again
#define LOG_DD_NOTE_MS (30u * 60u * 1000u)   // while dropping, a note about the count this often

typedef struct {
    uint32_t hash;
    uint32_t last_ms;      // last time this message was seen
    uint32_t note_ms;      // last time a note was produced for it
    uint32_t suppressed;   // dropped since the last note
    uint16_t burst;
    bool used;
    char text[48];         // start of the message, for the note
} log_dd_slot_t;

typedef struct {
    log_dd_slot_t s[LOG_DD_SLOTS];
    uint8_t next;  // round-robin eviction
} log_dd_t;

// msg is the text after the "L (timestamp) " prefix; a trailing newline is ignored. now_ms is a
// wrapping millisecond clock. Returns true when the line should be logged. When a note about
// dropped repeats is due, note receives its text (e.g. `412 repeats of "tag: message"`), otherwise
// note[0] = '\0'; the caller logs the note whatever the return value.
bool log_dd_pass(log_dd_t *d, const char *msg, uint32_t now_ms, char *note, size_t note_size);
