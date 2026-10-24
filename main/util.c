// util.c — pure string/descriptor helpers extracted verbatim from main.c so they can
// be unit-tested on the host (tests/host/). No ESP-IDF headers: these must stay
// freestanding C (C11) and be callable from both the firmware and the host tests.
#include "util.h"

#include <stdio.h>
#include <string.h>

size_t hid_output_report_len(const uint8_t *d, size_t n)
{
    uint32_t size = 0, count = 0;
    uint64_t bits = 0;
    size_t i = 0;
    while (i < n) {
        const uint8_t b = d[i];
        if (b == 0xFE) {  // long item: [0xFE][bDataSize][bLongItemTag][data]
            if (i + 1 >= n) {
                return 0;
            }
            i += 3 + d[i + 1];
            continue;
        }
        const size_t len = (b & 3) == 3 ? 4 : (b & 3);
        if (i + 1 + len > n) {
            return 0;
        }
        uint32_t v = 0;
        for (size_t k = 0; k < len; k++) {
            v |= (uint32_t)d[i + 1 + k] << (8 * k);
        }
        switch (b & 0xFC) {
        case 0x74:  // Report Size (global)
            size = v;
            break;
        case 0x94:  // Report Count (global)
            count = v;
            break;
        case 0x90:  // Output (main)
            bits += (uint64_t)size * count;
            break;
        case 0x84:  // Report ID
        case 0xA4:  // Push
        case 0xB4:  // Pop
            return 0;
        default:
            break;
        }
        i += 1 + len;
    }
    return bits > 8 * 0xFFFF ? 0xFFFF : (size_t)((bits + 7) / 8);
}

bool log_redact_token(char *line)
{
    bool redacted = false;
    for (char *p = strstr(line, "token="); p; p = strstr(p, "token=")) {
        p += 6;
        char *end = p;
        while (*end && *end != '&' && *end != '"' && *end != ' ' && *end != '\r' && *end != '\n') {
            end++;
        }
        if (end - p >= 3) {
            memmove(p + 3, end, strlen(end) + 1);  // shrinks or keeps the length: never overflows
            memcpy(p, "***", 3);
            p += 3;
            redacted = true;
        } else if (end > p) {
            memset(p, '*', end - p);
            p = end;
            redacted = true;
        }
    }
    return redacted;
}

bool url_encode(char *out, size_t size, const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (; *src; src++) {
        const unsigned char c = (unsigned char)*src;
        const bool plain = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           c == '-' || c == '.' || c == '_' || c == '~';
        if (n + (plain ? 1 : 3) >= size) {
            return false;
        }
        if (plain) {
            out[n++] = c;
        } else {
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 0xF];
        }
    }
    out[n] = '\0';
    return true;
}

bool token_equal(const char *given, const char *token)
{
    const size_t n = strlen(token);
    const size_t m = strlen(given);
    unsigned char diff = m != n;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned char)((i < m ? given[i] : 0) ^ token[i]);
    }
    return diff == 0;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

void url_decode(char *s)
{
    char *out = s;
    for (; *s; s++) {
        // '%' must be followed by exactly two hex digits (no spaces or signs); anything else
        // stays as typed. s[1] is checked first so s[2] is never read past the NUL.
        const int hi = *s == '%' ? hex_val(s[1]) : -1;
        const int lo = hi >= 0 ? hex_val(s[2]) : -1;
        if (hi >= 0 && lo >= 0) {
            *out++ = (char)(hi << 4 | lo);
            s += 2;
        } else {
            *out++ = *s == '+' ? ' ' : *s;
        }
    }
    *out = '\0';
}

void json_escape(char *out, size_t size, const char *src)
{
    if (size == 0) {
        return;  // no room even for the terminator
    }
    size_t n = 0;
    for (; *src && n + 7 < size; src++) {
        const unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            out[n++] = '\\';
            out[n++] = c;
        } else if (c < 0x20) {
            n += snprintf(out + n, size - n, "\\u%04x", c);
        } else {
            out[n++] = c;
        }
    }
    out[n] = '\0';
}

void log_mark_truncated(char *line, size_t size)
{
    static const char mark[] = "...\n";
    if (size >= sizeof(mark)) {
        memcpy(line + size - sizeof(mark), mark, sizeof(mark));
    }
}

bool auth_limiter_blocked(const auth_limiter_t *l, int64_t now_ms, uint32_t *retry_s)
{
    if (now_ms >= l->locked_until_ms) {
        return false;
    }
    if (retry_s) {
        *retry_s = (uint32_t)((l->locked_until_ms - now_ms + 999) / 1000);
    }
    return true;
}

void auth_limiter_fail(auth_limiter_t *l, int64_t now_ms, uint32_t max_fails)
{
    if (l->fails < UINT32_MAX) {
        l->fails++;
    }
    if (max_fails == 0) {
        max_fails = 1;
    }
    if (l->fails < max_fails) {
        return;
    }
    uint32_t shift = l->fails - max_fails;  // 0 on the first lockout
    int64_t window_ms = 30000;
    while (shift-- > 0 && window_ms < 15 * 60 * 1000) {
        window_ms *= 2;
    }
    if (window_ms > 15 * 60 * 1000) {
        window_ms = 15 * 60 * 1000;
    }
    l->locked_until_ms = now_ms + window_ms;
}

void auth_limiter_ok(auth_limiter_t *l)
{
    l->fails = 0;
    l->locked_until_ms = 0;
}

bool rate_gate(int64_t *last_ms, int64_t now_ms, int64_t min_gap_ms)
{
    if (now_ms - *last_ms < min_gap_ms && now_ms >= *last_ms) {
        return false;
    }
    *last_ms = now_ms;
    return true;
}
