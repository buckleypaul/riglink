/* tests/test_regression.c — regressions for the riglink code-review fixes.
 *
 * Isolated binary: it defines its own RIG_FN commands and a hand-rolled
 * rig_cmd, so keeping them out of the other test files avoids polluting their
 * rig.list output / registry.
 *
 * The RIG_FN bodies below take a single (unused) `int` argument purely so the
 * macro expansion has a non-empty __VA_ARGS__ — a zero-argument RIG_FN trips
 * -Wvariadic-macro-arguments-omitted (see the two known warnings in
 * tests/test_macros.c) and this file must stay warning-clean. The argument is
 * functionally irrelevant to what each test exercises. */
#include "riglink_test.h"
#include "riglink.h"
#include "../src/riglink_internal.h"
#include <string.h>

/* ---- commands exercised below ----------------------------------------- */

/* FIX 1: a RIG_FN that buffers a field then asserts — the buffered field must
 * NOT leak into this command's error response, nor into the next command. */
RIG_FN(void, emit_then_assert, int) { (void)arg0; rig_emit("leaked", (int64_t)999); RIG_ASSERT(0); }

/* FIX 2: rig_emit before AND after a mid-body event — both fields must land in
 * the response, and the event line must not be corrupted / abort. The trailing
 * comma in RIG_EMIT_EVENT("mid",) supplies an (empty) pair list without
 * tripping -Wvariadic-macro-arguments-omitted; it expands identically to
 * RIG_EMIT_EVENT("mid"). */
RIG_FN(void, mid_event, int) {
    (void)arg0;
    rig_emit("a", (int64_t)1);
    RIG_EMIT_EVENT("mid",);
    rig_emit("b", (int64_t)2);
}

/* FIX 2 (corollary): rig_emit before an event but NOT after — the buffered
 * field must still appear and the response must stay valid (no trailing comma). */
RIG_FN(void, emit_then_event, int) { (void)arg0; rig_emit("a", (int64_t)1); RIG_EMIT_EVENT("ev",); }

/* FIX 4: far more rig_emit output than the extra-fields buffer can hold — the
 * response must still be valid JSON (no mid-token cutoff) and flag the overflow. */
RIG_FN(void, flood, int) { (void)arg0; for (int i = 0; i < 100; i++) rig_emit("k", (int64_t)i); }

/* arg_too_long: a `str` arg longer than RIG_STR_ARG_SIZE-1 must surface a clear
 * arg_too_long error rather than dispatch a silently-truncated value. This fn
 * echoes the received length so a fitting value is observably *not* truncated. */
RIG_FN(int, take_str, str) { return (int)strlen(arg0); }

static void run_line(const char *line) {
    struct rig_tokens t;
    if (rig_tokenize(line, &t) != 0) { rig_io_err_syntax(t.name, "syntax error"); return; }
    rig_dispatch(&t);
}

/* ---- FIX 1: buffered extra fields must not leak across commands after an assert -- */
RIG_TEST(reg_assert_does_not_leak_extra_fields) {
    run_line("emit_then_assert 0");
    {
        const char *out = rig_mock_out();
        RIG_CHECK(strstr(out, "\"cmd\":\"emit_then_assert\"") != NULL);
        RIG_CHECK(strstr(out, "\"code\":\"assert\"") != NULL);
        RIG_CHECK(strstr(out, "leaked") == NULL);          /* buffered field discarded */
    }
    rig_mock_reset();
    run_line("rig.echo hi");
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"rig.echo\",\"ret\":[\"hi\"]}\n");
    RIG_CHECK(strstr(rig_mock_out(), "leaked") == NULL);   /* no splice into next command */
}

/* ---- FIX 2: rig_emit works before, between, and after a mid-body event ------- */
RIG_TEST(reg_emit_around_mid_body_event) {
    run_line("mid_event 0");
    const char *out = rig_mock_out();
    const char *ev  = strstr(out, "{\"event\":\"mid\"}");
    const char *res = strstr(out, "{\"cmd\":\"mid_event\",\"a\":1,\"b\":2,\"ret\":null}");
    RIG_CHECK(ev  != NULL);
    RIG_CHECK(res != NULL);
    RIG_CHECK(ev != NULL && res != NULL && ev < res);   /* event precedes the response */
}

RIG_TEST(reg_emit_before_event_no_emit_after) {
    run_line("emit_then_event 0");
    const char *out = rig_mock_out();
    RIG_CHECK(strstr(out, "{\"event\":\"ev\"}") != NULL);
    RIG_CHECK(strstr(out, "{\"cmd\":\"emit_then_event\",\"a\":1,\"ret\":null}") != NULL);
}

/* ---- FIX 3: rig_register is idempotent (no cycle, even after head displacement) -- */
static void reg__noop_tramp(const char *cmd, int argc, char **argv) {
    (void)argc; (void)argv; rig_io_resp_begin(cmd); rig_io_ret_void(); rig_io_line_end();
}
RIG_TEST(reg_register_is_idempotent) {
    static struct rig_cmd c        = { "c",        reg__noop_tramp, "void", NULL, 0, NULL, false };
    static struct rig_cmd displace  = { "displace", reg__noop_tramp, "void", NULL, 0, NULL, false };

    rig_register(&c);            /* c becomes head; c.next == NULL */
    rig_register(&displace);     /* displace becomes head; c displaced (still c.next == NULL) */
    rig_register(&c);            /* re-register the displaced node — must be a no-op, not a cycle */

    /* If a cycle existed these would loop forever; the test simply has to return. */
    RIG_CHECK(rig_cmd_find("unknown") == NULL);
    RIG_CHECK(rig_cmd_find("c") == &c);
    RIG_CHECK(rig_cmd_find("displace") == &displace);

    /* dispatch still works after the re-register */
    run_line("c");
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"c\",\"ret\":null}\n");
}

/* ---- FIX 4: extra-fields buffer overflow yields valid JSON, not a cutoff ----- */
static bool reg__is_balanced_json_line(const char *line, size_t len) {
    /* crude structural check: matched {}/[], non-negative depth, closes cleanly. */
    if (len == 0) return false;
    if (line[0] != '{' || line[len - 1] != '}') return false;
    int depth = 0;
    for (size_t i = 0; i < len; i++) {
        char ch = line[i];
        if (ch == '{' || ch == '[') depth++;
        else if (ch == '}' || ch == ']') { depth--; if (depth < 0) return false; }
    }
    return depth == 0;
}
RIG_TEST(reg_extra_fields_overflow_is_valid_json) {
    run_line("flood 0");
    const char *out = rig_mock_out();
    /* the line is: <sentinel>{...}\n  — locate the JSON payload */
    const char *brace = strchr(out, '{');
    RIG_CHECK(brace != NULL);
    const char *nl = brace ? strchr(brace, '\n') : NULL;
    RIG_CHECK(nl != NULL);
    if (brace && nl) {
        size_t len = (size_t)(nl - brace);
        RIG_CHECK(reg__is_balanced_json_line(brace, len));
    }
    RIG_CHECK(strstr(out, "\"_emit_overflow\":true") != NULL);
    RIG_CHECK(strstr(out, "\"cmd\":\"flood\"") != NULL);

    /* a following command's response is unaffected */
    rig_mock_reset();
    run_line("rig.echo ok");
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"rig.echo\",\"ret\":[\"ok\"]}\n");
}

/* ---- str arg overflow must be a clear error, not a silent truncation -------- */
RIG_TEST(reg_str_arg_overflow_is_arg_too_long) {
    /* RIG_STR_ARG_SIZE defaults to 64 → max usable length is 63. */
    char fits[64];   memset(fits, 'a', 63); fits[63] = '\0';        /* exactly 63 */
    char over[80];   memset(over, 'b', 64); over[64] = '\0';        /* 64 → overflows */
    char line[128];

    /* a value that fits dispatches normally and is NOT truncated (ret == 63) */
    snprintf(line, sizeof line, "take_str %s", fits);
    run_line(line);
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"take_str\",\"ret\":63}\n");

    /* a value one char too long → arg_too_long, with cmd / arg / got / max */
    rig_mock_reset();
    snprintf(line, sizeof line, "take_str %s", over);
    run_line(line);
    RIG_CHECK_STR_EQ(rig_mock_out(),
        "\x1eRIG {\"cmd\":\"take_str\",\"error\":{\"code\":\"arg_too_long\",\"arg\":0,\"got\":64,\"max\":63}}\n");

    /* the over-long command did not dispatch the body with a truncated value:
     * a following command's response is clean */
    rig_mock_reset();
    run_line("rig.echo ok");
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"rig.echo\",\"ret\":[\"ok\"]}\n");
}

RIG_TEST_MAIN()
