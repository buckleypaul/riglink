#include "riglink_test.h"
#include "riglink.h"
#include "../src/riglink_internal.h"
#include <string.h>

/* exposed existing functions */
static int add2(int a, int b)            { return a + b; }
static void do_reboot(void)              { /* nothing */ }
static int parse_len(const char *s)      { return (int)strlen(s); }
RIG_EXPOSE(int, add2, int, int);
RIG_EXPOSE(void, do_reboot);
RIG_EXPOSE(int, parse_len, str);

/* custom functions */
RIG_FN(int, scale, int, float) { return (int)(arg0 * arg1); }
RIG_FN(void, dump_state) { rig_emit("uptime", (int64_t)1234); rig_emit("mode", "idle"); }
RIG_FN(int, check_thing, int) { RIG_ASSERT(arg0 == 1); return arg0 * 10; }

/* exposed variable */
static int32_t g_level = 7;
RIG_EXPOSE_VAR(int32_t, g_level);

static void run_line(const char *line) {
    struct rig_tokens t; if (rig_tokenize(line, &t) != 0) { rig_io_err_syntax(t.name, "syntax error"); return; }
    rig_dispatch(&t);
}

RIG_TEST(macro_expose_int) {
    run_line("add2 2 3");
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"add2\",\"ret\":5}\n");
}
RIG_TEST(macro_expose_void) {
    run_line("do_reboot");
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"do_reboot\",\"ret\":null}\n");
}
RIG_TEST(macro_expose_str_arg) {
    run_line("parse_len \"hello world\"");
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"parse_len\",\"ret\":11}\n");
}
RIG_TEST(macro_expose_bad_arity) {
    run_line("add2 1");
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"add2\",\"error\":{\"code\":\"arg_count\",\"expected\":2,\"got\":1}}\n");
}
RIG_TEST(macro_expose_bad_arg) {
    run_line("add2 1 x");
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"add2\",\"error\":{\"code\":\"bad_args\",\"arg\":1,\"expected\":\"int\"}}\n");
}
RIG_TEST(macro_fn_with_float) {
    run_line("scale 3 2.5");
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"scale\",\"ret\":7}\n");
}
RIG_TEST(macro_fn_emit_fields) {
    run_line("dump_state");
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"dump_state\",\"uptime\":1234,\"mode\":\"idle\",\"ret\":null}\n");
}
RIG_TEST(macro_fn_assert_pass) {
    run_line("check_thing 1");
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"check_thing\",\"ret\":10}\n");
}
RIG_TEST(macro_fn_assert_fail) {
    run_line("check_thing 2");
    const char *out = rig_mock_out();
    RIG_CHECK(strstr(out, "\"cmd\":\"check_thing\"") != NULL);
    RIG_CHECK(strstr(out, "\"code\":\"assert\"") != NULL);
    RIG_CHECK(strstr(out, "\"msg\":\"arg0 == 1\"") != NULL);
    RIG_CHECK(strstr(out, "\"ret\"") == NULL);       /* no ret on assert failure */
}
RIG_TEST(macro_expose_var_get_set) {
    run_line("g_level.get");
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"g_level.get\",\"ret\":7}\n");
    rig_mock_reset();
    run_line("g_level.set 42");
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"g_level.set\",\"ret\":null}\n");
    RIG_CHECK_INT_EQ(g_level, 42);
    rig_mock_reset();
    run_line("g_level.get");
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"g_level.get\",\"ret\":42}\n");
}
RIG_TEST(macro_emit_event_immediate) {
    /* `(bool)true`: in C11 the <stdbool.h> macro `true` is `int 1` and would
     * dispatch the _Generic to rig_emit_i64 (emitting `1`); cast it so it hits
     * the `bool:` branch. (jcon.h documents the same caveat.) */
    RIG_EMIT_EVENT("rx_done", "len", (int64_t)12, "crc_ok", (bool)true);
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"event\":\"rx_done\",\"len\":12,\"crc_ok\":true}\n");
}
RIG_TEST(macro_emit_event_from_isr) {
    RIG_EMIT_EVENT_FROM_ISR("tick", "n", 5);
    rig_io_event_flush();
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"event\":\"tick\",\"n\":5}\n");
}

RIG_TEST_MAIN()
