/* src/riglink_internal.h — private to riglink's src/. The public API (rig_parse_*,
 * rig_emit_*, rig_io_*, rig_io_event_*, rig_log, struct rig_cmd, the RIG_* macros,
 * the event-ring types) lives in riglink.h; this header carries only the items
 * that stay internal to the implementation. */
#ifndef RIGLINK_INTERNAL_H
#define RIGLINK_INTERNAL_H

#include "riglink.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Compile-time config: Kconfig provides CONFIG_RIGLINK_*; the native/standalone
 * build provides plain defaults here. */
#ifdef CONFIG_RIGLINK_LINE_BUF_SIZE
#  define RIG_LINE_BUF_SIZE        CONFIG_RIGLINK_LINE_BUF_SIZE
#else
#  define RIG_LINE_BUF_SIZE        128
#endif
#ifdef CONFIG_RIGLINK_MAX_ARGS
#  define RIG_MAX_ARGS             CONFIG_RIGLINK_MAX_ARGS
#else
#  define RIG_MAX_ARGS             8
#endif
#ifdef CONFIG_RIGLINK_SCRATCH_SIZE
#  define RIG_SCRATCH_SIZE         CONFIG_RIGLINK_SCRATCH_SIZE
#else
#  define RIG_SCRATCH_SIZE         128
#endif
#ifdef CONFIG_RIGLINK_EVENT_QUEUE_DEPTH
#  define RIG_EVENT_QUEUE_DEPTH    CONFIG_RIGLINK_EVENT_QUEUE_DEPTH
#else
#  define RIG_EVENT_QUEUE_DEPTH    8
#endif
#ifdef CONFIG_RIGLINK_EXTRA_FIELDS_MAX
#  define RIG_EXTRA_FIELDS_MAX     CONFIG_RIGLINK_EXTRA_FIELDS_MAX
#else
#  define RIG_EXTRA_FIELDS_MAX     16     /* max buffered rig_emit() calls per RIG_FN body */
#endif
#ifdef CONFIG_RIGLINK_EXTRA_FIELDS_CAP
#  define RIG_EXTRA_FIELDS_CAP     CONFIG_RIGLINK_EXTRA_FIELDS_CAP
#else
#  define RIG_EXTRA_FIELDS_CAP     512    /* byte arena for those fields' name/value strings */
#endif
/* The on-wire line prefix: the 0x1e (RS) byte (always emitted here — Kconfig
 * strings can't carry a raw control byte) followed by an ASCII tag. Default
 * full prefix: "\x1eRIG ".
 *   - non-Zephyr: override the whole prefix by -DRIGLINK_SENTINEL='"\x1eFOO "'
 *   - Zephyr:     customize only the tag via CONFIG_RIGLINK_SENTINEL_TEXT      */
#if defined(RIGLINK_SENTINEL)
#  define RIG_SENTINEL             RIGLINK_SENTINEL
#elif defined(CONFIG_RIGLINK_SENTINEL_TEXT)
#  define RIG_SENTINEL             "\x1e" CONFIG_RIGLINK_SENTINEL_TEXT
#else
#  define RIG_SENTINEL             "\x1eRIG "
#endif
#if defined(CONFIG_RIGLINK_MINIFY) || !defined(CONFIG_RIGLINK)
#  define RIG_MINIFY               true
#else
#  define RIG_MINIFY               false
#endif
#if defined(CONFIG_RIGLINK_MEM_ACCESS) || defined(RIGLINK_MEM_ACCESS)
#  define RIG_MEM_ACCESS 1
#else
#  define RIG_MEM_ACCESS 0
#endif

/* ---- line assembly ---------------------------------------------------- */

struct rig_line {
    char   buf[RIG_LINE_BUF_SIZE];
    size_t len;
    bool   overflow;     /* a too-long line was seen; drop bytes until '\n' */
};

void rig_line_init(struct rig_line *l);

/* Feed one input byte. Returns:
 *   0  — byte consumed, line not yet complete
 *   1  — a complete line is now in l->buf (NUL-terminated, no trailing '\n'),
 *        l->overflow tells whether it was truncated; caller must handle then
 *        call rig_line_init() (or rig_line_reset()) before feeding more
 *  Treat '\r' as whitespace inside lines; lines are terminated by '\n'. */
int rig_line_feed(struct rig_line *l, char c);

void rig_line_reset(struct rig_line *l);   /* same as rig_line_init */

/* ---- tokenizer -------------------------------------------------------- */

struct rig_tokens {
    char  *argv[RIG_MAX_ARGS + 1];   /* [0] = command name; [1..] = args */
    int    argc;                     /* number of args (NOT counting the name) */
    char  *name;                     /* == argv[0], or NULL if the line is blank/comment */
    char   storage[RIG_LINE_BUF_SIZE];  /* tokens point into here */
};

/* Tokenize one assembled line. Whitespace-separated; a token may be
 * "double-quoted" to include spaces, with \" and \\ escapes inside quotes.
 * Lines that are empty or start with '#' (after leading whitespace) set
 * t->name = NULL and t->argc = 0 and return 0. Returns:
 *    0  — ok (possibly a no-op blank/comment line)
 *   -1  — malformed quoting / too many args (caller emits a bad_args error;
 *         t->name is set if it was parsed before the error so the error can
 *         name the command, else NULL) */
int rig_tokenize(const char *line, struct rig_tokens *t);

/* ---- command registry (internal hooks) -------------------------------- */

struct rig_cmd *rig_cmd_find(const char *name);
struct rig_cmd *rig_cmd_list_head(void);     /* for rig.list iteration */

/* Dispatch one tokenized line. No-op if t->name == NULL. Emits unknown_cmd if
 * the name is not registered; otherwise calls the command's trampoline. */
void rig_dispatch(struct rig_tokens *t);

/* ---- application shims --------------------------------------------------- */
/* rig_putc / rig_getc / rig_reset are implemented by the application (or by
 * tests/riglink_test.h in the native test build) and declared in riglink.h
 * (included above), so the implementation files see a single canonical decl. */

/* ---- extra-field capture (internal hook) ------------------------------- */
/* Reset the rig_emit-during-RIG_FN-body capture buffer to empty/inactive.
 * Called once per command (top of rig_dispatch) so a fragment left behind by a
 * body that longjmp'd out via RIG_ASSERT cannot leak into the next response. */
void rig_io_extra_reset(void);

/* ---- ISR-deferred event ring (drain side) ----------------------------- */
/* Drain & emit all queued records. If any were dropped since the last flush,
 * the next emitted event includes "_dropped":<n> and the counter resets. */
void rig_io_event_flush(void);

/* IRQ lock shim: real on Zephyr, no-op elsewhere. */
#if defined(__ZEPHYR__)
#  include <zephyr/irq.h>
#  define RIG_IRQ_LOCK()      irq_lock()
#  define RIG_IRQ_UNLOCK(k)   irq_unlock(k)
typedef unsigned int rig_irq_key_t;
#else
typedef int rig_irq_key_t;
#  define RIG_IRQ_LOCK()      0
#  define RIG_IRQ_UNLOCK(k)   ((void)(k))
#endif

#endif /* RIGLINK_INTERNAL_H */
