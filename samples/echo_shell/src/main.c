/* samples/echo_shell — riglink driven by Zephyr's shell.
 *
 * Unlike samples/echo there is no main() poll loop, no rig_putc/rig_getc, and
 * no call to rig_init() / rig_run(): zephyr/shell_backend.c (compiled in by
 * CONFIG_RIGLINK_BACKEND_SHELL=y) brings up everything via SYS_INIT and
 * dispatches commands as subcommands of the "rig" shell command. */

/* Widen the per-`str`-argument stack buffer so the long-string commands below
 * can accept arguments approaching the input line length. RIG_STR_ARG_SIZE is
 * read by the macro layer, so it MUST be set before riglink.h is included.
 * Keep it <= CONFIG_RIGLINK_LINE_BUF_SIZE (raised to 200 in prj.conf) so a
 * token that fits the input line also fits the parsed-argument buffer. */
#define RIG_STR_ARG_SIZE 192

#include <riglink.h>

#include <zephyr/kernel.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

/* --- long-string / non-trivial handlers -----------------------------------
 *
 * These exist specifically to exercise three failure modes that the short,
 * no-str commands above never hit, so CI can catch regressions in them:
 *
 *   (a) shell precheck garbage / wrong argument count — driven from the host
 *       (test_echo_shell.py) by calling these with too few/many tokens.
 *   (b) RX ring-buffer overflow — a long `str` argument pushes a near-line-length
 *       command through the shell's IRQ-driven RX ring in one burst.
 *   (c) shell-thread stack pressure — `echo_str` declares a RIG_STR_ARG_SIZE
 *       (192 B) argument buffer plus a same-size local on the shell thread's
 *       stack, so an undersized CONFIG_SHELL_STACK_SIZE shows up as a crash
 *       here rather than silently. */

/* echo_str(str) -> void: round-trips a long string back under the `echo` field
 * (a `str` *return type* isn't supported — `str` is an argument-only keyword —
 * so the echo rides an emitted field, not `ret`), and reports its byte length
 * under `len`. The reply re-emits the whole argument, so this is the command
 * that proves a long str arg survives RX buffering, the tokenizer,
 * rig_parse_str(), and JSON-escaped emission end to end. */
RIG_FN(void, echo_str, str)
{
	/* A stack copy on the shell thread: deliberately non-trivial work on a
	 * RIG_STR_ARG_SIZE-wide local so stack sizing is actually exercised. */
	char buf[RIG_STR_ARG_SIZE];
	size_t n = strlen(arg0);
	if (n >= sizeof(buf)) {
		n = sizeof(buf) - 1;
	}
	memcpy(buf, arg0, n);
	buf[n] = '\0';
	rig_emit("echo", buf);
	rig_emit("len", (uint32_t)n);
}

/* hash_str(str) -> unsigned: a non-trivial handler that does real work — a
 * 32-bit FNV-1a hash over the (long) argument. No echo of the input, so it also
 * proves the long arg made it across without relying on the reply carrying it. */
RIG_FN(unsigned, hash_str, str)
{
	uint32_t h = 2166136261u; /* FNV offset basis */
	for (const char *p = arg0; *p; ++p) {
		h ^= (uint8_t)*p;
		h *= 16777619u; /* FNV prime */
	}
	rig_emit("len", (uint32_t)strlen(arg0));
	return (unsigned)h;
}

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
