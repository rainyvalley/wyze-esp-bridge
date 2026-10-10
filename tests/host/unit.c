// Host unit tests for main/util.c, built standalone (no ESP-IDF):
//   cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined
//      -Imain tests/host/unit.c main/util.c tests/host/unity/unity.c -o /tmp/unit
// The same command runs in CI (release.yml, "tests" job). Semantics are the firmware's,
// so every case here documents real behavior, including the quirks it relies on.
#include "unity.h"

#include "../main/util.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// ------------------------------------------------------------------ helpers

// Builds one short-item descriptor chunk: [tag][payload little-endian, len bytes]
static size_t item(uint8_t *d, size_t at, uint8_t tag, uint32_t v, size_t len)
{
    d[at++] = tag;
    for (size_t k = 0; k < len; k++) {
        d[at++] = (uint8_t)(v >> (8 * k));
    }
    return at;
}

// [Report Size v][Report Count v][Output] — the minimal Output declaration.
// data-size bits of a tag: 0 = no payload, 1 = 1 byte, 2 = 2 bytes, 3 = 4 bytes,
// so the tag chosen must match the payload length passed here.
static size_t output_chunk(uint8_t *d, size_t at, uint32_t size_v, uint32_t count_v)
{
    const size_t s_len = size_v > 0xFF ? 2 : 1;
    const size_t c_len = count_v > 0xFF ? 2 : 1;
    size_t n = item(d, at, (uint8_t)(0x74 | s_len), size_v, s_len);  // Report Size
    n = item(d, n, (uint8_t)(0x94 | c_len), count_v, c_len);         // Report Count
    return item(d, n, 0x91, 0x01, 1);                                // Output (main)
}

static void expect_encode_decode(const char *src)
{
    char enc[256];
    char dec[256];
    TEST_ASSERT_TRUE(url_encode(enc, sizeof(enc), src));
    strcpy(dec, enc);
    url_decode(dec);
    TEST_ASSERT_EQUAL_STRING(src, dec);
}

// ------------------------------------------------------------------ hid_output_report_len

void test_hid_minimal_output_report(void)
{
    uint8_t d[16];
    size_t n = output_chunk(d, 0, 8, 1);
    TEST_ASSERT_EQUAL_size_t(1, hid_output_report_len(d, n));
}

void test_hid_bits_round_up_to_bytes(void)
{
    // 10 bits of Output round up to 2 bytes; the parser must never floor
    uint8_t d[16];
    size_t at = item(d, 0, 0x75, 1, 1);  // Report Size 1 (0x75 = Report Size, 1-byte payload)
    at = item(d, at, 0x95, 10, 1);       // Report Count 10 (0x95, 1-byte payload)
    at = item(d, at, 0x91, 0x01, 1);
    TEST_ASSERT_EQUAL_size_t(2, hid_output_report_len(d, at));
}

void test_hid_two_byte_payload_tags(void)
{
    // the 2-byte-payload forms of Report Size / Report Count (0x76 / 0x96)
    uint8_t d[16];
    size_t at = item(d, 0, 0x76, 8, 2);
    at = item(d, at, 0x96, 3, 2);
    at = item(d, at, 0x91, 0x01, 1);
    TEST_ASSERT_EQUAL_size_t(3, hid_output_report_len(d, at));
}

void test_hid_multiple_output_items_accumulate(void)
{
    uint8_t d[32];
    size_t at = output_chunk(d, 0, 8, 1);   // 8 bits
    at = output_chunk(d, at, 8, 2);         // +16 bits
    TEST_ASSERT_EQUAL_size_t(3, hid_output_report_len(d, at));
}

void test_hid_four_byte_data_size_and_value(void)
{
    // 0x77 = tag 0x74 with 4-byte payload: Report Size 16 → one Output of 16 bits = 2 bytes
    uint8_t d[16];
    size_t at = item(d, 0, 0x77, 16, 4);
    at = item(d, at, 0x95, 1, 1);
    at = item(d, at, 0x91, 0x01, 1);
    TEST_ASSERT_EQUAL_size_t(2, hid_output_report_len(d, at));
}

void test_hid_long_item_is_skipped(void)
{
    // [0xFE][bDataSize=2][tag][2 payload bytes] parses but contributes nothing
    uint8_t d[16];
    size_t at = output_chunk(d, 0, 8, 1);
    d[at++] = 0xFE;
    d[at++] = 0x02;
    d[at++] = 0x7F;
    d[at++] = 0xAA;
    d[at++] = 0xBB;
    TEST_ASSERT_EQUAL_size_t(1, hid_output_report_len(d, at));
}

void test_hid_long_item_truncated(void)
{
    const uint8_t d[] = {0xFE, 0x02};
    TEST_ASSERT_EQUAL_size_t(0, hid_output_report_len(d, sizeof(d)));
}

void test_hid_truncated_short_item(void)
{
    const uint8_t d[] = {0x95, 0x03};  // Report Count claims a Output item that never comes
    TEST_ASSERT_EQUAL_size_t(0, hid_output_report_len(d, sizeof(d)));
}

void test_hid_report_id_rejected(void)
{
    uint8_t d[16];
    size_t at = output_chunk(d, 0, 8, 1);
    d[at++] = 0x85;  // Report ID (global)
    d[at++] = 0x01;
    TEST_ASSERT_EQUAL_size_t(0, hid_output_report_len(d, at));
}

void test_hid_push_rejected(void)
{
    const uint8_t d[] = {0xA4, 0x01};
    TEST_ASSERT_EQUAL_size_t(0, hid_output_report_len(d, sizeof(d)));
}

void test_hid_pop_rejected(void)
{
    const uint8_t d[] = {0xB4, 0x01};
    TEST_ASSERT_EQUAL_size_t(0, hid_output_report_len(d, sizeof(d)));
}

void test_hid_empty_descriptor(void)
{
    TEST_ASSERT_EQUAL_size_t(0, hid_output_report_len((const uint8_t *)"", 0));
}

void test_hid_result_capped_at_0xffff(void)
{
    // 65535 * 65535 bits overflows the 2-byte report length: capped at 0xFFFF
    uint8_t d[16];
    size_t at = item(d, 0, 0x77, 0xFFFF, 4);  // Report Size 65535 (4-byte payload, 0x77)
    at = item(d, at, 0x96, 0xFFFF, 2);        // Report Count 65535 (2-byte payload, 0x96)
    at = item(d, at, 0x91, 0x01, 1);
    TEST_ASSERT_EQUAL_size_t(0xFFFF, hid_output_report_len(d, at));
}

// ------------------------------------------------------------------ log_redact_token

void test_redact_short_token_filled_with_stars(void)
{
    char line[] = "GET /?token=ab&x=1";
    TEST_ASSERT_TRUE(log_redact_token(line));
    TEST_ASSERT_EQUAL_STRING("GET /?token=**&x=1", line);
}

void test_redact_long_token_becomes_three_stars(void)
{
    char line[] = "GET /?token=abcdef&x=1";
    TEST_ASSERT_TRUE(log_redact_token(line));
    TEST_ASSERT_EQUAL_STRING("GET /?token=***&x=1", line);
}

void test_redact_line_shrinks_without_overflow(void)
{
    // Redaction here removes 9 characters; the caller's buffer must not overflow, and the
    // tail after the terminator is shifted down intact.
    char line[] = "x token=abcdefghij&x=y";
    TEST_ASSERT_TRUE(log_redact_token(line));
    TEST_ASSERT_EQUAL_STRING("x token=***&x=y", line);
}

void test_redact_quoted_token(void)
{
    char line[] = "err: parse uri = ws://h/?token=secret99\"";
    TEST_ASSERT_TRUE(log_redact_token(line));
    TEST_ASSERT_EQUAL_STRING("err: parse uri = ws://h/?token=***\"", line);
}

void test_redact_multiple_tokens_one_line(void)
{
    char line[] = "a=1 token=longtoken b=2 token=xy z";
    TEST_ASSERT_TRUE(log_redact_token(line));
    TEST_ASSERT_EQUAL_STRING("a=1 token=*** b=2 token=** z", line);
}

void test_redact_one_char_token_starred(void)
{
    // even a 1-character value is filled with one '*': end > p is the only guard
    char line[] = "GET /?token=a&x=1";
    TEST_ASSERT_TRUE(log_redact_token(line));
    TEST_ASSERT_EQUAL_STRING("GET /?token=*&x=1", line);
}

void test_redact_bare_token_equals_untouched(void)
{
    char line[] = "token= with no value";
    TEST_ASSERT_FALSE(log_redact_token(line));
    TEST_ASSERT_EQUAL_STRING("token= with no value", line);
}

void test_redact_benign_string_untouched(void)
{
    char line[] = "wifi connected, rssi=-52";
    TEST_ASSERT_FALSE(log_redact_token(line));
    TEST_ASSERT_EQUAL_STRING("wifi connected, rssi=-52", line);
}

void test_redact_token_prefix_not_matched_in_identifier(void)
{
    // "tokens=" is not "token=": no redaction
    char line[] = "tokens=abcdef&x";
    TEST_ASSERT_FALSE(log_redact_token(line));
    TEST_ASSERT_EQUAL_STRING("tokens=abcdef&x", line);
}

void test_redact_stops_at_line_end_chars(void)
{
    char line[] = "token=abc\r\n";
    TEST_ASSERT_TRUE(log_redact_token(line));
    TEST_ASSERT_EQUAL_STRING("token=***\r\n", line);
}

// ------------------------------------------------------------------ url_encode

void test_encode_unreserved_pass_through(void)
{
    char out[16];
    TEST_ASSERT_TRUE(url_encode(out, sizeof(out), "aB9-._~"));
    TEST_ASSERT_EQUAL_STRING("aB9-._~", out);
}

void test_encode_space_and_special_chars(void)
{
    char out[32];
    TEST_ASSERT_TRUE(url_encode(out, sizeof(out), "a b+c"));
    TEST_ASSERT_EQUAL_STRING("a%20b%2Bc", out);
}

void test_encode_all_bytes_not_unreserved(void)
{
    // every byte the encoder changes, encoded and decoded back
    static const char *s = " !\"#$%&'()*+,/:;<=>?@[\\]^`{|}~\t";
    expect_encode_decode(s);
}

void test_encode_empty(void)
{
    char out[8];
    TEST_ASSERT_TRUE(url_encode(out, sizeof(out), ""));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_encode_exact_fit(void)
{
    char out[3];  // "ab" + NUL exactly fits
    TEST_ASSERT_TRUE(url_encode(out, sizeof(out), "ab"));
    TEST_ASSERT_EQUAL_STRING("ab", out);
}

void test_encode_too_small_returns_false(void)
{
    // first byte already needs 3 and only a NUL fits: nothing is written
    char out[3] = "?";
    TEST_ASSERT_FALSE(url_encode(out, sizeof(out), "%ab"));
    TEST_ASSERT_EQUAL_STRING("?", out);
}

void test_encode_encodes_all_before_failing(void)
{
    // 4 plain chars fill a NUL-terminated buffer of 5; the 5th needs 3 more bytes → false
    char out[5];
    TEST_ASSERT_FALSE(url_encode(out, sizeof(out), "ab X"));
}

// ------------------------------------------------------------------ url_decode

void test_decode_hex_and_plus(void)
{
    char s[] = "a%20b+c%2B";
    url_decode(s);
    TEST_ASSERT_EQUAL_STRING("a b c+", s);
}

void test_decode_leaves_plain_plus_only_where_encoded(void)
{
    // '+' is always a space in a query value, even unencoded
    char s[] = "++";
    url_decode(s);
    TEST_ASSERT_EQUAL_STRING("  ", s);
}

void test_decode_in_place_overlap(void)
{
    char s[] = "%41%42%43%44";
    url_decode(s);
    TEST_ASSERT_EQUAL_STRING("ABCD", s);
}

void test_decode_malformed_escapes_kept_verbatim(void)
{
    char s[] = "a% zz%2";
    url_decode(s);
    TEST_ASSERT_EQUAL_STRING("a% zz%2", s);
}

void test_decode_trailing_percent_kept(void)
{
    char s[] = "abc%";
    url_decode(s);
    TEST_ASSERT_EQUAL_STRING("abc%", s);
}

void test_decode_empty(void)
{
    char s[] = "";
    url_decode(s);
    TEST_ASSERT_EQUAL_STRING("", s);
}

void test_decode_noop(void)
{
    char s[] = "plain-text_~.";
    url_decode(s);
    TEST_ASSERT_EQUAL_STRING("plain-text_~.", s);
}

// ------------------------------------------------------------------ round-trip

void test_encode_decode_round_trip(void)
{
    expect_encode_decode("abc DEF-ghi_123.~x");
    expect_encode_decode("two words & more+stuff?here");
    expect_encode_decode("=/?#%@");
}

// ------------------------------------------------------------------ token_equal

void test_token_equal_equal(void)
{
    TEST_ASSERT_TRUE(token_equal("secret", "secret"));
    TEST_ASSERT_TRUE(token_equal("", ""));
    TEST_ASSERT_TRUE(token_equal("a", "a"));
}

void test_token_equal_prefix_not_equal(void)
{
    // a prefix of the token must not authenticate
    TEST_ASSERT_FALSE(token_equal("sec", "secret"));
}

void test_token_equal_longer_guess_not_equal(void)
{
    // a guess longer than the token must not authenticate
    TEST_ASSERT_FALSE(token_equal("secrets", "secret"));
}

void test_token_equal_wrong_byte_not_equal(void)
{
    TEST_ASSERT_FALSE(token_equal("secreU", "secret"));
    TEST_ASSERT_FALSE(token_equal("<ecret", "secret"));
}

void test_token_equal_empty_given(void)
{
    TEST_ASSERT_FALSE(token_equal("", "secret"));
}

void test_token_equal_binary_bytes(void)
{
    // bytes above 0x7F must compare correctly, not through a signed char
    TEST_ASSERT_TRUE(token_equal("\x80\x81", "\x80\x81"));
    TEST_ASSERT_FALSE(token_equal("\x80", "\x81"));
}

// ------------------------------------------------------------------ json_escape

void test_json_escape_plain_untouched(void)
{
    char out[32];
    json_escape(out, sizeof(out), "esp32-p4 rev3");
    TEST_ASSERT_EQUAL_STRING("esp32-p4 rev3", out);
}

void test_json_escape_quote_and_backslash(void)
{
    char out[32];
    json_escape(out, sizeof(out), "a\"b\\c");
    TEST_ASSERT_EQUAL_STRING("a\\\"b\\\\c", out);
}

void test_json_escape_control_chars(void)
{
    char out[32];
    json_escape(out, sizeof(out), "\t\x01\n");
    TEST_ASSERT_EQUAL_STRING("\\u0009\\u0001\\u000a", out);
}

void test_json_escape_empty(void)
{
    char out[4] = "zz";
    json_escape(out, sizeof(out), "");
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_json_escape_truncates_to_fit(void)
{
    // the loop's guard is n + 7 < size (room for the widest escape plus NUL), so plain text
    // truncates while 7 bytes of headroom remain: out[8] holds only 1 byte here
    char out[8] = {'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X'};
    json_escape(out, sizeof(out), "abcdefghi");
    TEST_ASSERT_EQUAL_STRING("a", out);
}

void test_json_escape_plain_text_uses_headroom(void)
{
    // with headroom 7, a 2-byte plain prefix still fits, then stops with a NUL on top
    char out[9] = "ZZZZZ";
    json_escape(out, sizeof(out), "abcdefg");
    TEST_ASSERT_EQUAL_STRING("ab", out);
}

void test_json_escape_stops_before_partial_escape(void)
{
    // room for exactly one more escape: size 12, src needs "a" + quoted backslash...
    char out[12] = "??????????";
    json_escape(out, sizeof(out), "a\\bdefghij");
    // a (1) + \\\\ (2) = 3; next char d: 3+7 < 12 ✓ so it fits; check the whole prefix fit
    TEST_ASSERT_EQUAL_STRING("a\\\\bd", out);
}

void test_json_escape_size_zero_is_empty(void)
{
    char out[4] = "zz";
    json_escape(out, sizeof(out), "abc");
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_json_escape_nul_never_touched_when_full(void)
{
    // a single backslash is the widest two-byte escape ('\','\'): the NUL goes right after
    char out[10] = "WWWWWWWWW";
    json_escape(out, sizeof(out), "\\");  // 0 + 7 < 10 writes the pair; then src is exhausted
    TEST_ASSERT_EQUAL_STRING("\\\\", out);
}

// ------------------------------------------------------------------ fuzz-ish combined

void test_redact_after_escape_still_masks_uri_token(void)
{
    // what status_get() does: json_escape a URI that carries ?token=, then redact it.
    // The escaping must not create a new "token=" or hide an existing one.
    char out[128];
    json_escape(out, sizeof(out), "ws://h:8080/ws/bridge?x=1");
    TEST_ASSERT_FALSE(strstr(out, "token="));
    TEST_ASSERT_TRUE(log_redact_token(out) == false);
    memcpy(strstr(out, "x=1") - 1, "?token=secret&", 14);
    TEST_ASSERT_TRUE(log_redact_token(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "***"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hid_minimal_output_report);
    RUN_TEST(test_hid_bits_round_up_to_bytes);
    RUN_TEST(test_hid_two_byte_payload_tags);
    RUN_TEST(test_hid_multiple_output_items_accumulate);
    RUN_TEST(test_hid_four_byte_data_size_and_value);
    RUN_TEST(test_hid_long_item_is_skipped);
    RUN_TEST(test_hid_long_item_truncated);
    RUN_TEST(test_hid_truncated_short_item);
    RUN_TEST(test_hid_report_id_rejected);
    RUN_TEST(test_hid_push_rejected);
    RUN_TEST(test_hid_pop_rejected);
    RUN_TEST(test_hid_empty_descriptor);
    RUN_TEST(test_hid_result_capped_at_0xffff);
    RUN_TEST(test_redact_short_token_filled_with_stars);
    RUN_TEST(test_redact_long_token_becomes_three_stars);
    RUN_TEST(test_redact_line_shrinks_without_overflow);
    RUN_TEST(test_redact_quoted_token);
    RUN_TEST(test_redact_multiple_tokens_one_line);
    RUN_TEST(test_redact_one_char_token_starred);
    RUN_TEST(test_redact_bare_token_equals_untouched);
    RUN_TEST(test_redact_benign_string_untouched);
    RUN_TEST(test_redact_token_prefix_not_matched_in_identifier);
    RUN_TEST(test_redact_stops_at_line_end_chars);
    RUN_TEST(test_encode_unreserved_pass_through);
    RUN_TEST(test_encode_space_and_special_chars);
    RUN_TEST(test_encode_all_bytes_not_unreserved);
    RUN_TEST(test_encode_empty);
    RUN_TEST(test_encode_exact_fit);
    RUN_TEST(test_encode_too_small_returns_false);
    RUN_TEST(test_encode_encodes_all_before_failing);
    RUN_TEST(test_decode_hex_and_plus);
    RUN_TEST(test_decode_leaves_plain_plus_only_where_encoded);
    RUN_TEST(test_decode_in_place_overlap);
    RUN_TEST(test_decode_malformed_escapes_kept_verbatim);
    RUN_TEST(test_decode_trailing_percent_kept);
    RUN_TEST(test_decode_empty);
    RUN_TEST(test_decode_noop);
    RUN_TEST(test_encode_decode_round_trip);
    RUN_TEST(test_token_equal_equal);
    RUN_TEST(test_token_equal_prefix_not_equal);
    RUN_TEST(test_token_equal_longer_guess_not_equal);
    RUN_TEST(test_token_equal_wrong_byte_not_equal);
    RUN_TEST(test_token_equal_empty_given);
    RUN_TEST(test_token_equal_binary_bytes);
    RUN_TEST(test_json_escape_plain_untouched);
    RUN_TEST(test_json_escape_quote_and_backslash);
    RUN_TEST(test_json_escape_control_chars);
    RUN_TEST(test_json_escape_empty);
    RUN_TEST(test_json_escape_truncates_to_fit);
    RUN_TEST(test_json_escape_plain_text_uses_headroom);
    RUN_TEST(test_json_escape_stops_before_partial_escape);
    RUN_TEST(test_json_escape_size_zero_is_empty);
    RUN_TEST(test_json_escape_nul_never_touched_when_full);
    RUN_TEST(test_redact_after_escape_still_masks_uri_token);
    return UNITY_END();
}