#include "riglink_test.h"
#include "riglink.h"
#include <string.h>

static int add2(int a, int b) { return a + b; }
RIG_EXPOSE(int, add2, int, int);
RIG_FN(int, ping_and_event, int) { RIG_EMIT_EVENT("got_ping", "seq", (int64_t)arg0); return 0; }

/* Run the pump until it returns false or no progress (input exhausted). */
static void pump_until_idle(void) {
    rig_init();
    int spins = 0;
    while (rig_run()) {
        if (g_rig_mock.in_pos >= g_rig_mock.in_len) break;   /* input exhausted */
        if (++spins > 10000) break;                          /* safety */
    }
}

RIG_TEST(e2e_ready_event_on_init) {
    rig_init();
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"event\":\"ready\",\"version\":\"0.1.0\"}\n");
}
RIG_TEST(e2e_request_response) {
    rig_mock_feed("add2 4 5\n");
    pump_until_idle();
    /* output = ready event, then the response */
    RIG_CHECK_STR_EQ(rig_mock_out(),
        "\x1eRIG {\"event\":\"ready\",\"version\":\"0.1.0\"}\n\x1eRIG {\"cmd\":\"add2\",\"ret\":9}\n");
}
RIG_TEST(e2e_blank_and_comment_ignored) {
    rig_mock_feed("\n# a comment\nadd2 1 2\n");
    pump_until_idle();
    RIG_CHECK(strstr(rig_mock_out(), "{\"cmd\":\"add2\",\"ret\":3}") != NULL);
}
RIG_TEST(e2e_unknown_command) {
    rig_mock_feed("nope 1\n");
    pump_until_idle();
    RIG_CHECK(strstr(rig_mock_out(), "{\"cmd\":\"nope\",\"error\":{\"code\":\"unknown_cmd\"}}") != NULL);
}
RIG_TEST(e2e_event_before_ret) {
    rig_mock_feed("ping_and_event 7\n");
    pump_until_idle();
    const char *out = rig_mock_out();
    const char *ev = strstr(out, "{\"event\":\"got_ping\",\"seq\":7}");
    const char *res = strstr(out, "{\"cmd\":\"ping_and_event\",\"ret\":0}");
    RIG_CHECK(ev != NULL && res != NULL && ev < res);
}
RIG_TEST(e2e_partial_line_then_rest) {
    rig_init();
    rig_mock_feed("add");
    RIG_CHECK(rig_run() == true);            /* consumes "add", no line yet */
    rig_mock_feed("2 8 9\n");
    while (rig_run()) { if (g_rig_mock.in_pos >= g_rig_mock.in_len) break; }
    RIG_CHECK(strstr(rig_mock_out(), "{\"cmd\":\"add2\",\"ret\":17}") != NULL);
}
RIG_TEST(e2e_deinit_stops_pump) {
    rig_init();
    rig_deinit();
    RIG_CHECK(rig_run() == false);
}

RIG_TEST_MAIN()
