/* samples/echo_shell — riglink driven by Zephyr's shell.
 *
 * Unlike samples/echo there is no main() poll loop, no rig_putc/rig_getc, and
 * no call to rig_init() / rig_run(): zephyr/shell_backend.c (compiled in by
 * CONFIG_RIGLINK_BACKEND_SHELL=y) brings up everything via SYS_INIT and
 * dispatches commands as subcommands of the "rig" shell command. */

#include <riglink.h>

#include <zephyr/kernel.h>

/* --- exposed surface (same as samples/echo so the same tests apply) --- */

static int add(int a, int b) { return a + b; }
RIG_EXPOSE(int, add, int, int);

RIG_FN(int, scale, int, float) { return (int)(arg0 * arg1); }

RIG_FN(void, board_info) {
	rig_emit("board", CONFIG_BOARD);
	rig_emit("uptime_ms", (int64_t)k_uptime_get());
}

static int32_t g_counter;
RIG_EXPOSE_VAR(int32_t, g_counter);

RIG_FN(int, check_even, int) { RIG_ASSERT((arg0 % 2) == 0); return arg0 / 2; }

/* periodic event from a timer ISR -> exercises the ISR-deferred event ring,
 * drained by the shell backend's k_work item */
static uint32_t g_tick;
static void tick_cb(struct k_timer *t) { (void)t; RIG_EMIT_EVENT_FROM_ISR("tick", "n", g_tick++); }
K_TIMER_DEFINE(g_ticker, tick_cb, NULL);

int main(void)
{
	k_timer_start(&g_ticker, K_MSEC(500), K_MSEC(500));
	return 0;
}
