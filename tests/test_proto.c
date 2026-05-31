#include "riglink_test.h"
#include "../src/riglink_internal.h"

/* Drive a NUL-terminated string through rig_line_feed; return the assembled
 * line for the first complete line seen, or NULL if none completed. Sets
 * *overflow if provided. */
static const char *feed_line(const char *s, bool *overflow) {
    static struct rig_line l;
    rig_line_init(&l);
    for (const char *p = s; *p; p++) {
        if (rig_line_feed(&l, *p) == 1) { if (overflow) *overflow = l.overflow; return l.buf; }
    }
    return NULL;
}

RIG_TEST(line_simple) {
    RIG_CHECK_STR_EQ(feed_line("foo 1 2\n", NULL), "foo 1 2");
}
RIG_TEST(line_strips_cr) {
    RIG_CHECK_STR_EQ(feed_line("foo 1 2\r\n", NULL), "foo 1 2");
}
RIG_TEST(line_empty) {
    RIG_CHECK_STR_EQ(feed_line("\n", NULL), "");
}
RIG_TEST(line_overflow_truncates_and_flags) {
    char big[RIG_LINE_BUF_SIZE + 50];
    memset(big, 'x', sizeof big);
    big[sizeof big - 1] = '\0';
    /* append newline */
    char s[sizeof big + 1];
    snprintf(s, sizeof s, "%s\n", big);
    bool ov = false;
    const char *line = feed_line(s, &ov);
    RIG_CHECK(line != NULL);
    RIG_CHECK(ov == true);
    RIG_CHECK(strlen(line) == RIG_LINE_BUF_SIZE - 1);   /* fully filled minus NUL */
}
RIG_TEST(line_two_in_a_row) {
    static struct rig_line l;
    rig_line_init(&l);
    const char *s = "a\nb\n";
    int got = 0;
    for (const char *p = s; *p; p++) {
        if (rig_line_feed(&l, *p) == 1) {
            got++;
            RIG_CHECK_STR_EQ(l.buf, got == 1 ? "a" : "b");
            rig_line_reset(&l);
        }
    }
    RIG_CHECK_INT_EQ(got, 2);
}

RIG_TEST(tok_basic) {
    struct rig_tokens t;
    RIG_CHECK_INT_EQ(rig_tokenize("foo 1 2", &t), 0);
    RIG_CHECK_STR_EQ(t.name, "foo");
    RIG_CHECK_INT_EQ(t.argc, 2);
    RIG_CHECK_STR_EQ(t.argv[1], "1");
    RIG_CHECK_STR_EQ(t.argv[2], "2");
}
RIG_TEST(tok_extra_whitespace) {
    struct rig_tokens t;
    RIG_CHECK_INT_EQ(rig_tokenize("  foo   1\t 2  ", &t), 0);
    RIG_CHECK_STR_EQ(t.name, "foo");
    RIG_CHECK_INT_EQ(t.argc, 2);
}
RIG_TEST(tok_no_args) {
    struct rig_tokens t;
    RIG_CHECK_INT_EQ(rig_tokenize("rig.list", &t), 0);
    RIG_CHECK_STR_EQ(t.name, "rig.list");
    RIG_CHECK_INT_EQ(t.argc, 0);
}
RIG_TEST(tok_quoted_with_spaces) {
    struct rig_tokens t;
    RIG_CHECK_INT_EQ(rig_tokenize("greet \"hello world\" 7", &t), 0);
    RIG_CHECK_INT_EQ(t.argc, 2);
    RIG_CHECK_STR_EQ(t.argv[1], "hello world");
    RIG_CHECK_STR_EQ(t.argv[2], "7");
}
RIG_TEST(tok_quoted_escapes) {
    struct rig_tokens t;
    RIG_CHECK_INT_EQ(rig_tokenize("p \"a\\\"b\\\\c\"", &t), 0);
    RIG_CHECK_STR_EQ(t.argv[1], "a\"b\\c");
}
RIG_TEST(tok_blank_line) {
    struct rig_tokens t;
    RIG_CHECK_INT_EQ(rig_tokenize("   ", &t), 0);
    RIG_CHECK(t.name == NULL);
    RIG_CHECK_INT_EQ(t.argc, 0);
}
RIG_TEST(tok_comment) {
    struct rig_tokens t;
    RIG_CHECK_INT_EQ(rig_tokenize("  # foo 1 2", &t), 0);
    RIG_CHECK(t.name == NULL);
}
RIG_TEST(tok_unterminated_quote) {
    struct rig_tokens t;
    RIG_CHECK_INT_EQ(rig_tokenize("foo \"abc", &t), -1);
}
RIG_TEST(tok_too_many_args) {
    struct rig_tokens t;
    /* RIG_MAX_ARGS default 8 → 9 args is too many */
    RIG_CHECK_INT_EQ(rig_tokenize("f 1 2 3 4 5 6 7 8 9", &t), -1);
}

RIG_TEST(parse_bool_ok) {
    bool b;
    RIG_CHECK(rig_parse_bool("true", &b) && b == true);
    RIG_CHECK(rig_parse_bool("false", &b) && b == false);
    RIG_CHECK(!rig_parse_bool("1", &b));
    RIG_CHECK(!rig_parse_bool("TRUE", &b));
}
RIG_TEST(parse_i64_ok) {
    int64_t v;
    RIG_CHECK(rig_parse_i64("0", &v) && v == 0);
    RIG_CHECK(rig_parse_i64("-22", &v) && v == -22);
    RIG_CHECK(rig_parse_i64("0x10", &v) && v == 16);
    RIG_CHECK(rig_parse_i64("-0x1F", &v) && v == -31);
    RIG_CHECK(rig_parse_i64("9223372036854775807", &v) && v == INT64_MAX);
    RIG_CHECK(!rig_parse_i64("", &v));
    RIG_CHECK(!rig_parse_i64("12x", &v));
    RIG_CHECK(!rig_parse_i64("9223372036854775808", &v));   /* overflow */
}
RIG_TEST(parse_u64_ok) {
    uint64_t v;
    RIG_CHECK(rig_parse_u64("0", &v) && v == 0);
    RIG_CHECK(rig_parse_u64("0xFFFFFFFFFFFFFFFF", &v) && v == UINT64_MAX);
    RIG_CHECK(!rig_parse_u64("-1", &v));
}
RIG_TEST(parse_double_ok) {
    double d;
    RIG_CHECK(rig_parse_double("3.5", &d) && d == 3.5);
    RIG_CHECK(rig_parse_double("-0.25", &d) && d == -0.25);
    RIG_CHECK(rig_parse_double("10", &d) && d == 10.0);
    RIG_CHECK(!rig_parse_double("3.5q", &d));
    RIG_CHECK(!rig_parse_double("", &d));
}
RIG_TEST(parse_i_range_narrowing) {
    int64_t v;
    RIG_CHECK(rig_parse_i_range("127", -128, 127, &v) && v == 127);
    RIG_CHECK(!rig_parse_i_range("128", -128, 127, &v));     /* int8 overflow */
    RIG_CHECK(rig_parse_i_range("-128", -128, 127, &v) && v == -128);
}
RIG_TEST(parse_u_range_narrowing) {
    uint64_t v;
    RIG_CHECK(rig_parse_u_range("255", 255, &v) && v == 255);
    RIG_CHECK(!rig_parse_u_range("256", 255, &v));            /* uint8 overflow */
}

RIG_TEST_MAIN()
