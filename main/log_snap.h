// Format helpers for the log tail that is saved to flash so it survives restarts: a CRC'd header
// per slot, a wrap-safe sequence number to find the newest slot, and the arithmetic for packing
// "previous run + restart marker + this run" into one slot. The flash I/O itself is in main.c.
// No ESP-IDF dependencies: unit-tested.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOGSNAP_MAGIC 0x31474C57u  // "WLG1"

typedef struct {
    uint32_t magic;
    uint32_t seq;  // newer snapshots have larger numbers (wrap-safe, see logsnap_newer)
    uint32_t len;  // bytes of text that follow the header
    uint32_t crc;  // CRC-32 of that text
} logsnap_hdr_t;

#define LOGSNAP_HDR_SIZE ((size_t)sizeof(logsnap_hdr_t))

// zlib-style CRC-32: logsnap_crc32(logsnap_crc32(0, a, na), b, nb) == logsnap_crc32(0, a+b, na+nb).
uint32_t logsnap_crc32(uint32_t crc, const void *data, size_t len);

// Magic matches and the length is possible; the CRC still has to be checked against the text.
bool logsnap_hdr_plausible(const logsnap_hdr_t *h, size_t text_max);

// True when sequence number a is newer than b.
bool logsnap_newer(uint32_t a, uint32_t b);

typedef struct {
    size_t prev;    // bytes taken from the end of the previous run's text
    size_t marker;  // bytes of the restart marker (its end)
    size_t cur;     // bytes taken from the end of this run's log
    bool cut;       // something older did not fit
} logsnap_layout_t;

// Packs previous + marker + current into cap bytes, keeping the newest: all of cur first, then
// the marker, then as much of the end of prev as still fits.
logsnap_layout_t logsnap_layout(size_t prev_len, size_t marker_len, size_t cur_len, size_t cap);

// After cutting the front off a text, how many bytes to skip so it starts at a whole line.
size_t logsnap_line_start(const char *text, size_t len);
