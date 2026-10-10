// util.h — pure helpers extracted from main.c so they can be unit-tested on the host
// (tests/host/). Keep these freestanding C (C11): no ESP-IDF headers, and logic changes
// belong in the matching test cases first.
#ifndef WYZE_UTIL_H
#define WYZE_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Size in bytes of the Output report declared by a HID report descriptor, for a device without
// report IDs (SET_REPORT goes out with report ID 0). Returns 0 when it can't tell: report IDs,
// Push/Pop, or a truncated descriptor.
size_t hid_output_report_len(const uint8_t *d, size_t n);

// Masks the value of every "token=" in a log line (*** when it spills, * per char when
// shorter). Returns true when anything was redacted.
bool log_redact_token(char *line);

// Percent-encodes src for a URI query value (RFC 3986 unreserved characters pass through, so
// typical alphanumeric tokens are sent unchanged). Returns false if out is too small.
bool url_encode(char *out, size_t size, const char *src);

// Compares without an early exit, so the time taken does not reveal how much of a guess matched.
bool token_equal(const char *given, const char *token);

// Decodes %XX and '+' in a query value in place.
void url_decode(char *s);

// Writes src as the contents of a JSON string (without quotes), truncating to fit.
void json_escape(char *out, size_t size, const char *src);

#endif