/* src/riglink_core.c — riglink lifecycle and the rig_run() pump. */
#include "riglink.h"
#include "riglink_internal.h"

#include <setjmp.h>

jmp_buf     rig__assert_jmp;             /* used by RIG_ASSERT */
const char *rig__cmd_active = NULL;      /* current command name for RIG_ASSERT */

static struct rig_line g_rig_line;
static bool            g_rig_running;

int rig_init(void) {
    rig_line_init(&g_rig_line);
    g_rig_running = true;
    /* announce readiness so a freshly (re)booted device is recognisable */
    rig_io_event_begin("ready");
    rig_emit_str("version", RIGLINK_VERSION_STRING);
    rig_io_event_end();
    return 0;
}

void rig_deinit(void) { g_rig_running = false; }

bool rig_run(void) {
    if (!g_rig_running) return false;

    rig_io_event_flush();                /* drain any ISR-deferred events */

    /* Bound work per call. The 2x covers the worst case: we may scan a full
     * just-completed line's worth of bytes plus another line's worth of bytes
     * toward the next '\n' before bailing out for the one-command-per-call rule. */
    int budget = 2 * RIG_LINE_BUF_SIZE;
    while (budget-- > 0) {
        int ch = rig_getc();
        if (ch < 0) break;               /* no data available right now */
        if (rig_line_feed(&g_rig_line, (char)ch) == 1) {
            struct rig_tokens t;
            if (g_rig_line.overflow) {
                /* still tokenize so we can name the command if it fit */
                (void)rig_tokenize(g_rig_line.buf, &t);
                rig_io_err_overflow(t.name);
            } else {
                int rc = rig_tokenize(g_rig_line.buf, &t);
                if (rc != 0) rig_io_err_syntax(t.name, "syntax error");  /* malformed quoting / too many args */
                else         rig_dispatch(&t);
            }
            rig_line_reset(&g_rig_line);
            break;                       /* one command per rig_run() call */
        }
    }
    return true;
}

#if defined(CONFIG_RIGLINK_THREAD)
/* With CONFIG_RIGLINK_THREAD=y a dedicated thread owns the pump (calls rig_init()
 * once and then rig_run() in a loop). The application must NOT also call
 * rig_init() / rig_run() itself in that configuration. */
#include <zephyr/kernel.h>
static void rig__thread_entry(void *a, void *b, void *c) {
    (void)a; (void)b; (void)c;
    rig_init();
    while (rig_run()) k_msleep(1);
    rig_deinit();
}
K_THREAD_DEFINE(rig_thread, CONFIG_RIGLINK_THREAD_STACK_SIZE, rig__thread_entry,
                NULL, NULL, NULL, CONFIG_RIGLINK_THREAD_PRIO, 0, 0);
#endif
