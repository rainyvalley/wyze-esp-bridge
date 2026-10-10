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

void url_decode(char *s)
{
    char *out = s;
    for (; *s; s++) {
        int hi, lo;
        if (*s == '%' && sscanf(s + 1, "%1x%1x", &hi, &lo) == 2) {
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