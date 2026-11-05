#include "log_snap.h"

uint32_t logsnap_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

bool logsnap_hdr_plausible(const logsnap_hdr_t *h, size_t text_max)
{
    return h->magic == LOGSNAP_MAGIC && h->len > 0 && h->len <= text_max;
}

bool logsnap_newer(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

logsnap_layout_t logsnap_layout(size_t prev_len, size_t marker_len, size_t cur_len, size_t cap)
{
    logsnap_layout_t l = {0};
    l.cur = cur_len < cap ? cur_len : cap;
    size_t rem = cap - l.cur;
    l.marker = marker_len < rem ? marker_len : rem;
    rem -= l.marker;
    l.prev = prev_len < rem ? prev_len : rem;
    l.cut = l.cur < cur_len || l.marker < marker_len || l.prev < prev_len;
    return l;
}

size_t logsnap_line_start(const char *text, size_t len)
{
    const size_t scan = len < 256 ? len : 256;
    for (size_t i = 0; i < scan; i++) {
        if (text[i] == '\n') {
            return i + 1;
        }
    }
    return 0;
}
