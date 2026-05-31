#include "riglink_test.h"
#include "riglink.h"
#include "../src/riglink_internal.h"

/* A hand-rolled command for the test: "add a b" -> {"cmd":"add","ret":a+b} */
static void tramp_add(const char *cmd, int argc, char **argv) {
    if (argc != 2) { rig_io_err_argcount(cmd, 2, argc); return; }
    int64_t a, b;
    if (!rig_parse_i64(argv[0], &a)) { rig_io_err_badarg(cmd, 0, "int"); return; }
    if (!rig_parse_i64(argv[1], &b)) { rig_io_err_badarg(cmd, 1, "int"); return; }
    rig_io_resp_begin(cmd);
    rig_emit_i64("ret", a + b);
    rig_io_line_end();
}
static const char *const add_argtypes[] = { "int", "int" };
static struct rig_cmd cmd_add = { "add", tramp_add, "int", add_argtypes, 2, NULL, false };

/* Register it once for this test file. */
__attribute__((constructor)) static void reg_add(void) { rig_register(&cmd_add); }

/* Helper: feed a request line through tokenize + dispatch. */
static void run_line(const char *line) {
    struct rig_tokens t;
    int rc = rig_tokenize(line, &t);
    if (rc != 0) { rig_io_err_syntax(t.name, "syntax error"); return; }
    rig_dispatch(&t);
}

RIG_TEST(cmd_find_registered) {
    RIG_CHECK(rig_cmd_find("add") == &cmd_add);
    RIG_CHECK(rig_cmd_find("nope") == NULL);
}
RIG_TEST(cmd_dispatch_calls_trampoline) {
    run_line("add 2 3");
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"add\",\"ret\":5}\n");
}
RIG_TEST(cmd_dispatch_unknown) {
    run_line("frobnicate 1");
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"frobnicate\",\"error\":{\"code\":\"unknown_cmd\"}}\n");
}
RIG_TEST(cmd_dispatch_bad_arity) {
    run_line("add 1");
    RIG_CHECK_STR_EQ(rig_mock_out(),
        "\x1eRIG {\"cmd\":\"add\",\"error\":{\"code\":\"arg_count\",\"expected\":2,\"got\":1}}\n");
}
RIG_TEST(cmd_dispatch_bad_arg) {
    run_line("add 1 x");
    RIG_CHECK_STR_EQ(rig_mock_out(),
        "\x1eRIG {\"cmd\":\"add\",\"error\":{\"code\":\"bad_args\",\"arg\":1,\"expected\":\"int\"}}\n");
}
RIG_TEST(cmd_dispatch_blank_is_noop) {
    run_line("   ");
    RIG_CHECK_STR_EQ(rig_mock_out(), "");
}

RIG_TEST(builtin_echo) {
    run_line("rig.echo hello \"big world\" 7");
    RIG_CHECK_STR_EQ(rig_mock_out(),
        "\x1eRIG {\"cmd\":\"rig.echo\",\"ret\":[\"hello\",\"big world\",\"7\"]}\n");
}
RIG_TEST(builtin_echo_no_args) {
    run_line("rig.echo");
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"rig.echo\",\"ret\":[]}\n");
}
RIG_TEST(builtin_reset) {
    run_line("rig.reset");
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"rig.reset\",\"ret\":null}\n");
    RIG_CHECK_INT_EQ(g_rig_mock.reset_count, 1);
}
RIG_TEST(builtin_list_includes_add_and_builtins) {
    run_line("rig.list");
    const char *out = rig_mock_out();
    /* It is a {"cmd":"rig.list","cmds":[...],"ret":null} line. Spot-check entries. */
    RIG_CHECK(strstr(out, "\"cmd\":\"rig.list\"") != NULL);
    RIG_CHECK(strstr(out, "\"ret\":null") != NULL);
    RIG_CHECK(strstr(out, "{\"name\":\"add\",\"ret\":\"int\",\"args\":[\"int\",\"int\"]}") != NULL);
    RIG_CHECK(strstr(out, "{\"name\":\"rig.echo\",\"ret\":\"array\",\"args\":[\"...\"]}") != NULL);
    RIG_CHECK(strstr(out, "{\"name\":\"rig.reset\",\"ret\":\"void\",\"args\":[]}") != NULL);
}

#if RIG_MEM_ACCESS
static uint8_t g_peek_target[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
RIG_TEST(builtin_peek) {
    char line[64];
    snprintf(line, sizeof line, "rig.peek 0x%llx 4", (unsigned long long)(uintptr_t)g_peek_target);
    run_line(line);
    /* bytes returned as a hex string under "ret" */
    RIG_CHECK(strstr(rig_mock_out(), "\"ret\":\"deadbeef\"") != NULL);
}
RIG_TEST(builtin_poke) {
    static uint32_t target = 0;
    char line[64];
    snprintf(line, sizeof line, "rig.poke 0x%llx 0x11223344", (unsigned long long)(uintptr_t)&target);
    run_line(line);
    RIG_CHECK_STR_EQ(rig_mock_out(), "\x1eRIG {\"cmd\":\"rig.poke\",\"ret\":null}\n");
    RIG_CHECK_INT_EQ(target, 0x11223344u);
}
#endif

RIG_TEST_MAIN()
