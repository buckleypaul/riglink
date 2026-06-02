/* zephyr/shell_backend.c — bridge between Zephyr's shell and riglink's command
 * registry. Activated by CONFIG_RIGLINK_BACKEND_SHELL.
 *
 * With this backend: the application does NOT implement rig_putc / rig_getc
 * and does NOT call rig_init() / rig_run(). Zephyr's shell owns the UART (so
 * RX is IRQ-driven and won't drop bytes); each riglink command is exposed as a
 * subcommand of a root "rig" command; response bytes are written through
 * shell_fprintf() to the shell instance that dispatched the command.
 *
 * Concurrency: the shell processes one command at a time on its own thread, so
 * a static `active_shell` pointer is race-free *within* a dispatch. The
 * delayable work item that drains the ISR-deferred event ring runs on the
 * system workqueue, so a small mutex serialises event flushes against in-flight
 * command output — otherwise an event line could splice itself into the middle
 * of a response line. */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>

#include <errno.h>
#include <string.h>

#include "riglink.h"
#include "../src/riglink_internal.h"

/* Set by rig_sub_handler() while a command is dispatching. NULL otherwise
 * (events flushed from the work item, plus the boot-time "ready" event, fall
 * through to the global UART shell). */
static const struct shell *active_shell;

/* Serialises command output and event-ring flushes against each other. Held
 * across the entire dispatch (including the user function body), not just the
 * response-line emission — pushing the lock down into the line layer would
 * require Zephyr deps in riglink core. With the default `tick` rate (2 Hz) in
 * the sample this is invisible; heavy tick fan-in would warrant the refactor. */
static K_MUTEX_DEFINE(rig_io_lock);

static const struct shell *putc_target(void)
{
	return active_shell ? active_shell : shell_backend_uart_get_ptr();
}

int rig_putc(char c)
{
	const struct shell *sh = putc_target();
	if (sh) {
		shell_fprintf(sh, SHELL_NORMAL, "%c", c);
	}
	return 0;
}

int rig_getc(void)
{
	return -1;
}

__weak void rig_reset(void)
{
	sys_reboot(SYS_REBOOT_COLD);
}

/* ---------------- subcommand registration ---------------------------------- */

static const char *strip_rig_prefix(const char *name)
{
	return strncmp(name, "rig.", 4) == 0 ? name + 4 : name;
}

static int rig_sub_handler(const struct shell *sh, size_t argc, char **argv);

static void rig_dyn_get(size_t idx, struct shell_static_entry *entry)
{
	size_t i = 0;
	struct rig_cmd *c = rig_cmd_list_head();
	while (c != NULL && i < idx) {
		c = c->next;
		i++;
	}
	if (c == NULL) {
		entry->syntax = NULL;
		return;
	}
	entry->syntax  = strip_rig_prefix(c->name);
	entry->handler = rig_sub_handler;
	entry->subcmd  = NULL;
	/* help is unused (CONFIG_SHELL_HELP=n in the sample); avoid wiring
	 * c->ret here since it's a return-type string, not help text. */
	entry->help    = NULL;
}

SHELL_DYNAMIC_CMD_CREATE(rig_subcmds, rig_dyn_get);

static int rig_sub_handler(const struct shell *sh, size_t argc, char **argv)
{
	/* User commands (RIG_EXPOSE / RIG_FN) register without the "rig." prefix,
	 * so argv[0] matches their registered name directly — try that first.
	 * Built-ins (rig.echo / rig.list / rig.reset) carry the prefix, which
	 * rig_dyn_get strips from the shell syntax, so re-attach it on miss. */
	struct rig_cmd *c = rig_cmd_find(argv[0]);
	char prefixed[RIG_LINE_BUF_SIZE];
	if (c == NULL) {
		int n = snprintk(prefixed, sizeof(prefixed), "rig.%s", argv[0]);
		if (n < 0 || n >= (int)sizeof(prefixed)) {
			/* Frame the error so the host always gets a parseable response
			 * instead of a plain-text shell_error() line (which would only
			 * surface as an opaque RiglinkTimeout). */
			k_mutex_lock(&rig_io_lock, K_FOREVER);
			active_shell = sh;
			rig_io_extra_reset();
			rig_io_err_syntax(argv[0], "subcommand name too long");
			active_shell = NULL;
			k_mutex_unlock(&rig_io_lock);
			return -EINVAL;
		}
		c = rig_cmd_find(prefixed);
	}

	/* Build a rig_tokens so rig_dispatch's prologue (extra_reset +
	 * unknown-cmd error) stays the single source of truth. argv pointers
	 * live in the shell's parse buffer for the duration of this call, so
	 * we can hand them straight through without using t.storage[]. */
	struct rig_tokens t = {0};
	t.name = c ? c->name : argv[0];
	t.argv[0] = (char *)t.name;
	int passed = (int)argc - 1;
	if (passed > RIG_MAX_ARGS) {
		passed = RIG_MAX_ARGS;
	}
	for (int i = 0; i < passed; i++) {
		t.argv[i + 1] = argv[i + 1];
	}
	t.argc = passed;

	k_mutex_lock(&rig_io_lock, K_FOREVER);
	active_shell = sh;
	rig_dispatch(&t);
	active_shell = NULL;
	k_mutex_unlock(&rig_io_lock);
	return 0;
}

/* Root handler for the "rig" command. Zephyr's shell invokes this when the
 * token after "rig" does not match any dynamic subcommand (a typo, or a command
 * the firmware was built without) — without it, the shell prints a plain-text
 * "rig: ... not found" line that is NOT sentinel-framed, so the host sees only
 * an opaque RiglinkTimeout. By emitting a framed unknown_cmd envelope here, the
 * host ALWAYS gets a parseable response, matching the poll backend's behavior
 * (samples/echo) for unknown commands. A bare "rig" with no subcommand reports
 * the missing command name as "rig". */
static int rig_root_handler(const struct shell *sh, size_t argc, char **argv)
{
	/* argv[0] is "rig"; argv[1] (if present) is the unmatched subcommand. */
	const char *bad = (argc > 1) ? argv[1] : argv[0];
	k_mutex_lock(&rig_io_lock, K_FOREVER);
	active_shell = sh;
	rig_io_extra_reset();
	rig_io_err_unknown_cmd(bad);
	active_shell = NULL;
	k_mutex_unlock(&rig_io_lock);
	return 0;
}

SHELL_CMD_REGISTER(rig, &rig_subcmds, "riglink command tree", rig_root_handler);

/* ---------------- event-ring drain ----------------------------------------- */

static void rig_evt_work_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(rig_evt_work, rig_evt_work_fn);

static void rig_evt_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	k_mutex_lock(&rig_io_lock, K_FOREVER);
	rig_io_event_flush();
	k_mutex_unlock(&rig_io_lock);
	k_work_schedule(&rig_evt_work,
			K_MSEC(CONFIG_RIGLINK_SHELL_EVENT_POLL_MS));
}

/* ---------------- bring-up ------------------------------------------------- */

static int rig_shell_init(void)
{
	/* APPLICATION priority runs after the shell UART backend's POST_KERNEL
	 * init, so shell_fprintf() to the UART shell is usable here. The "ready"
	 * event emitted by rig_init() lands on the global UART shell because
	 * active_shell is still NULL. */
	rig_init();
	k_work_schedule(&rig_evt_work, K_MSEC(CONFIG_RIGLINK_SHELL_EVENT_POLL_MS));
	return 0;
}
SYS_INIT(rig_shell_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
