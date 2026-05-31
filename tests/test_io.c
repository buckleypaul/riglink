#include "riglink_test.h"
#include "../src/riglink_internal.h"

#include <math.h>

RIG_TEST(io_empty_object) {
    rig_io_line_begin();
    rig_io_line_end();
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {}\n");
}
RIG_TEST(io_response_with_int_ret) {
    rig_io_resp_begin("foo");
    rig_emit_i64("ret", 0);
    rig_io_line_end();
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"foo\",\"ret\":0}\n");
}
RIG_TEST(io_response_void) {
    rig_io_resp_begin("device_reboot");
    rig_io_ret_void();
    rig_io_line_end();
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"device_reboot\",\"ret\":null}\n");
}
RIG_TEST(io_response_with_emitted_fields) {
    rig_io_resp_begin("dump");
    rig_emit_i64("uptime", 1234);
    rig_emit_str("mode", "idle");
    rig_io_ret_void();
    rig_io_line_end();
    RIG_CHECK_STR_EQ(rig_mock_out(),
        "\x1eRIG {\"cmd\":\"dump\",\"uptime\":1234,\"mode\":\"idle\",\"ret\":null}\n");
}
RIG_TEST(io_str_is_escaped) {
    rig_io_resp_begin("get");
    rig_emit_str("ret", "a\"b\\c\nd\te");
    rig_io_line_end();
    RIG_CHECK_STR_EQ(rig_mock_out(),
        "\x1eRIG {\"cmd\":\"get\",\"ret\":\"a\\\"b\\\\c\\nd\\te\"}\n");
}
RIG_TEST(io_double_nonfinite_is_null) {
    /* jcon's %g would emit bare inf/-inf/nan (invalid JSON); rig_emit_double
     * must mirror the host's finite-only rule and degrade to null instead. */
    rig_io_line_begin();
    rig_emit_double("x", INFINITY);
    rig_io_line_end();
    const char *out = rig_mock_out();
    RIG_CHECK(strstr(out, "\"x\":null") != NULL);
    RIG_CHECK(strstr(out, "inf") == NULL);
    RIG_CHECK(strstr(out, "nan") == NULL);
}
RIG_TEST(io_double_finite_is_numeric) {
    rig_io_line_begin();
    rig_emit_double("x", 1.5);
    rig_io_line_end();
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"x\":1.5}\n");
}
RIG_TEST(io_putc_failure_is_swallowed) {
    g_rig_mock.out_fail = true;
    rig_io_resp_begin("foo");
    rig_emit_i64("ret", 0);
    rig_io_line_end();           /* must not crash; jcon latches the I/O error */
    RIG_CHECK_STR_EQ(rig_mock_out(), "");
}

RIG_TEST(io_err_unknown_cmd) {
    rig_io_err_unknown_cmd("nope");
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"nope\",\"error\":{\"code\":\"unknown_cmd\"}}\n");
}
RIG_TEST(io_err_argcount) {
    rig_io_err_argcount("foo", 2, 1);
    RIG_CHECK_STR_EQ(rig_mock_out(),
        "\x1eRIG {\"cmd\":\"foo\",\"error\":{\"code\":\"arg_count\",\"expected\":2,\"got\":1}}\n");
}
RIG_TEST(io_err_badarg) {
    rig_io_err_badarg("foo", 1, "int");
    RIG_CHECK_STR_EQ(rig_mock_out(),
        "\x1eRIG {\"cmd\":\"foo\",\"error\":{\"code\":\"bad_args\",\"arg\":1,\"expected\":\"int\"}}\n");
}
RIG_TEST(io_err_overflow_no_cmd) {
    rig_io_err_overflow(NULL);
    RIG_CHECK_STR_EQ(rig_mock_out(),
        "\x1eRIG {\"cmd\":\"?\",\"error\":{\"code\":\"bad_args\",\"msg\":\"line too long\"}}\n");
}
RIG_TEST(io_err_assert) {
    rig_io_err_assert("check_thing", "src/main.c", 42, "x == 1");
    RIG_CHECK_STR_EQ(rig_mock_out(),
        "\x1eRIG {\"cmd\":\"check_thing\",\"error\":{\"code\":\"assert\",\"file\":\"src/main.c\",\"line\":42,\"msg\":\"x == 1\"}}\n");
}

RIG_TEST(io_event_immediate) {
    rig_io_event_begin("rx_done");
    rig_emit_i64("len", 12);
    rig_emit_bool("crc_ok", true);
    rig_io_event_end();
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"event\":\"rx_done\",\"len\":12,\"crc_ok\":true}\n");
}
RIG_TEST(io_event_no_fields) {
    rig_io_event_begin("timer_fired");
    rig_io_event_end();
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"event\":\"timer_fired\"}\n");
}
RIG_TEST(io_log) {
    rig_log("got %d packets", 3);
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"log\":\"got 3 packets\"}\n");
}
RIG_TEST(io_event_ring_drain) {
    struct rig_evt_rec r1 = { .name = "a", .npairs = 1,
        .pairs = { { .key = "n", .type = RIG_EVT_I64, .i = 1 } } };
    struct rig_evt_rec r2 = { .name = "b", .npairs = 0 };
    rig_io_event_enqueue(&r1);
    rig_io_event_enqueue(&r2);
    rig_io_event_flush();
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"event\":\"a\",\"n\":1}\n\x1eRIG {\"event\":\"b\"}\n");
}
RIG_TEST(io_event_ring_overflow_reports_dropped) {
    /* RIG_EVENT_QUEUE_DEPTH defaults to 8; enqueue 10, expect 2 dropped */
    for (int i = 0; i < 10; i++) {
        struct rig_evt_rec r = { .name = "e", .npairs = 1,
            .pairs = { { .key = "i", .type = RIG_EVT_I64, .i = i } } };
        rig_io_event_enqueue(&r);
    }
    rig_io_event_flush();
    /* first emitted event carries "_dropped":2 (the 2 oldest, i=0 and i=1, were dropped) */
    const char *out = rig_mock_out();
    RIG_CHECK(strstr(out, "\"_dropped\":2") != NULL);
    RIG_CHECK(strstr(out, "\"i\":2") != NULL);     /* i=2 survived */
    RIG_CHECK(strstr(out, "\"i\":9") != NULL);     /* i=9 survived */
    RIG_CHECK(strstr(out, "\"i\":0") == NULL);     /* i=0 dropped */
}

RIG_TEST_MAIN()
