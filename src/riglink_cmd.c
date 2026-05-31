/* src/riglink_cmd.c — command registry, dispatch, built-ins. */
#include "riglink.h"
#include "riglink_internal.h"

#include <string.h>

static struct rig_cmd *g_rig_cmds;     /* intrusive list head */

void rig_register(struct rig_cmd *cmd) {
    /* Idempotent for a given node. The old "cmd->next != NULL || head == cmd"
     * test missed the case where a node was registered first (so its next is
     * NULL) and then displaced from the head by later registrations — relinking
     * it would create a cycle and make rig_cmd_find / rig.list loop forever.
     * A dedicated flag is unambiguous. */
    if (cmd->registered) return;
    cmd->registered = true;
    cmd->next = g_rig_cmds;
    g_rig_cmds = cmd;
}

struct rig_cmd *rig_cmd_find(const char *name) {
    for (struct rig_cmd *c = g_rig_cmds; c; c = c->next)
        if (strcmp(c->name, name) == 0) return c;
    return NULL;
}
struct rig_cmd *rig_cmd_list_head(void) { return g_rig_cmds; }

void rig_dispatch(struct rig_tokens *t) {
    if (t->name == NULL) return;                       /* blank / comment */
    /* Clear any extra-fields fragment left behind by a previous command (e.g.
     * one whose RIG_FN body longjmp'd out via RIG_ASSERT, skipping the
     * trampoline's rig__io_extra_end()). Runs once per command, unconditionally,
     * before any built-in or trampoline can splice it into its response. */
    rig_io_extra_reset();
    struct rig_cmd *c = rig_cmd_find(t->name);
    if (!c) { rig_io_err_unknown_cmd(t->name); return; }
    c->fn(t->name, t->argc, &t->argv[1]);              /* argv[1..] = arguments */
}

/* ---- built-ins -------------------------------------------------------- */

static void rig__bi_echo(const char *cmd, int argc, char **argv) {
    rig_io_resp_begin(cmd);
    jcon_array_start("ret");
    for (int i = 0; i < argc; i++) rig_emit_str(NULL, argv[i]);
    jcon_array_end();
    rig_io_line_end();
}
static const char *const rig__echo_argtypes[] = { "..." };
static struct rig_cmd rig__cmd_echo = { "rig.echo", rig__bi_echo, "array", rig__echo_argtypes, RIG_NARGS_VARIADIC, NULL, false };
__attribute__((constructor)) static void rig__reg_echo(void) { rig_register(&rig__cmd_echo); }

static void rig__bi_reset(const char *cmd, int argc, char **argv) {
    (void)argc; (void)argv;
    rig_io_resp_begin(cmd);
    rig_io_ret_void();
    rig_io_line_end();
    rig_reset();          /* application-provided; typically reboots */
}
static struct rig_cmd rig__cmd_reset = { "rig.reset", rig__bi_reset, "void", NULL, 0, NULL, false };
__attribute__((constructor)) static void rig__reg_reset(void) { rig_register(&rig__cmd_reset); }

static void rig__bi_list(const char *cmd, int argc, char **argv) {
    (void)argc; (void)argv;
    rig_io_resp_begin(cmd);
    jcon_array_start("cmds");
    for (struct rig_cmd *c = rig_cmd_list_head(); c; c = c->next) {
        jcon_object_start(NULL);
        rig_emit_str("name", c->name);
        rig_emit_str("ret", c->ret ? c->ret : "void");
        jcon_array_start("args");
        if (c->nargs == RIG_NARGS_VARIADIC) {
            rig_emit_str(NULL, "...");
        } else {
            for (int i = 0; i < c->nargs; i++) rig_emit_str(NULL, c->argtypes ? c->argtypes[i] : "?");
        }
        jcon_array_end();
        jcon_object_end();
    }
    jcon_array_end();
    rig_io_ret_void();
    rig_io_line_end();
}
static struct rig_cmd rig__cmd_list = { "rig.list", rig__bi_list, "void", NULL, 0, NULL, false };
__attribute__((constructor)) static void rig__reg_list(void) { rig_register(&rig__cmd_list); }

#if RIG_MEM_ACCESS
/* rig.peek <addr> <len> -> {"cmd":"rig.peek","ret":"<hex bytes>"} (len capped). */
static void rig__bi_peek(const char *cmd, int argc, char **argv) {
    if (argc != 2) { rig_io_err_argcount(cmd, 2, argc); return; }
    uint64_t addr, len;
    if (!rig_parse_u64(argv[0], &addr)) { rig_io_err_badarg(cmd, 0, "uintptr_t"); return; }
    if (!rig_parse_u64(argv[1], &len))  { rig_io_err_badarg(cmd, 1, "size_t"); return; }
    if (len > RIG_SCRATCH_SIZE / 2) len = RIG_SCRATCH_SIZE / 2;   /* cap */
    rig_io_resp_begin(cmd);
    jcon_add_bytes_hex("ret", (const void *)(uintptr_t)addr, (size_t)len);
    rig_io_line_end();
}
/* rig.poke <addr> <val> -> writes a 32-bit word; {"cmd":"rig.poke","ret":null}. */
static void rig__bi_poke(const char *cmd, int argc, char **argv) {
    if (argc != 2) { rig_io_err_argcount(cmd, 2, argc); return; }
    uint64_t addr, val;
    if (!rig_parse_u64(argv[0], &addr)) { rig_io_err_badarg(cmd, 0, "uintptr_t"); return; }
    if (!rig_parse_u64(argv[1], &val))  { rig_io_err_badarg(cmd, 1, "uint32_t"); return; }
    *(volatile uint32_t *)(uintptr_t)addr = (uint32_t)val;
    rig_io_resp_begin(cmd);
    rig_io_ret_void();
    rig_io_line_end();
}
static const char *const rig__peek_argtypes[] = { "uintptr_t", "size_t" };
static const char *const rig__poke_argtypes[] = { "uintptr_t", "uint32_t" };
static struct rig_cmd rig__cmd_peek = { "rig.peek", rig__bi_peek, "str", rig__peek_argtypes, 2, NULL, false };
static struct rig_cmd rig__cmd_poke = { "rig.poke", rig__bi_poke, "void", rig__poke_argtypes, 2, NULL, false };
__attribute__((constructor)) static void rig__reg_mem(void) { rig_register(&rig__cmd_peek); rig_register(&rig__cmd_poke); }
#endif /* RIG_MEM_ACCESS */
