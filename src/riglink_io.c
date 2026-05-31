/* src/riglink_io.c — sentinel-framed JSON output via jcon. */
#include "riglink_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* RIG_SENTINEL begins with the 0x1e (RS) byte by construction (see
 * riglink_internal.h). This guards an empty tag, which would make every line
 * indistinguishable from a lone control byte. sizeof of a string-literal array
 * is a constant expression, so this is portable to GCC and Clang. */
_Static_assert(sizeof(RIG_SENTINEL) >= 3, "RIGLINK_SENTINEL_TEXT must be non-empty");

/* jcon's putc takes (void *ctx, char); adapt onto the app-provided rig_putc. */
static int rig__jcon_putc(void *ctx, char c) { (void)ctx; return rig_putc(c); }

static void rig__emit_sentinel(void) {
    for (const char *p = RIG_SENTINEL; *p; p++) (void)rig_putc(*p);
}

/* ---- extra-field capture (rig_emit calls from inside a RIG_FN body) ------ */
/* A RIG_FN body runs BEFORE its response line is opened, so any rig_emit() calls
 * it makes can't go straight to jcon (the response object doesn't exist yet) —
 * they're captured into a small fixed-size typed buffer here and replayed into
 * the response by rig_io_resp_begin(), between "cmd" and "ret".  This mirrors
 * the ISR event-ring style (struct rig_evt_pair / rig_evt_rec): a fixed array of
 * typed records plus a byte arena for the strings (the caller's `name`/value
 * pointers may not outlive the command, so they're copied into the arena).  No
 * jcon involvement and no byte surgery on jcon's output.
 *
 * Interleaving with events/logs/asserts emitted from inside the body: those go
 * to the transport immediately.  rig_io_line_begin() clears g_extra_active so the
 * about-to-be-emitted line's rig_emit_* calls reach jcon; rig_io_line_end()
 * restores g_extra_active = g_extra_in_body so subsequent rig_emit() calls in the
 * same body resume buffering.  RIG_ASSERT longjmps out of the trampoline without
 * calling rig__io_extra_end(); rig_io_extra_reset() (run at the top of every
 * dispatch) clears the buffered fields so they can't leak into the next command.
 *
 * On overflow (more fields than the array holds, or strings exceeding the arena)
 * capture stops and rig_io_resp_begin() emits "_emit_overflow":true instead of
 * the buffered fields. */

/* RIG_EXTRA_FIELDS_MAX / RIG_EXTRA_FIELDS_CAP come from riglink_internal.h
 * (Kconfig-tunable, with plain defaults for the standalone build). */

struct rig_extra_field {
    const char *name;               /* points into g_extra_arena */
    enum { EX_I64, EX_U64, EX_BOOL, EX_DBL, EX_STR, EX_NULL } type;
    union {                         /* exactly one is live, per `type` */
        int64_t     i;              /* EX_I64; also EX_BOOL (stored 0/1) */
        uint64_t    u;              /* EX_U64 */
        double      d;              /* EX_DBL */
        const char *s;              /* EX_STR; points into g_extra_arena */
    };
};

static struct rig_extra_field g_extra[RIG_EXTRA_FIELDS_MAX];
static int    g_extra_n;            /* number of buffered fields */
static char   g_extra_arena[RIG_EXTRA_FIELDS_CAP];
static size_t g_extra_arena_len;
static bool   g_extra_active;       /* rig_emit_* should buffer (not call jcon) */
static bool   g_extra_in_body;      /* a RIG_FN body is currently executing */
static bool   g_extra_used;         /* capture was begun this command (replay buffer) */
static bool   g_extra_truncated;    /* the field array or the arena overflowed */

/* Copy a NUL-terminated string into the arena; return a pointer to the copy, or
 * NULL (and set g_extra_truncated) if it doesn't fit. */
static const char *rig__extra_dup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    if (g_extra_arena_len + n > sizeof g_extra_arena) { g_extra_truncated = true; return NULL; }
    char *dst = g_extra_arena + g_extra_arena_len;
    memcpy(dst, s, n);
    g_extra_arena_len += n;
    return dst;
}

/* Allocate the next field slot, with `name` copied into the arena. Returns NULL
 * (and sets g_extra_truncated) if the array is full or the name doesn't fit. */
static struct rig_extra_field *rig__extra_new(const char *name) {
    if (g_extra_truncated) return NULL;
    if (g_extra_n >= RIG_EXTRA_FIELDS_MAX) { g_extra_truncated = true; return NULL; }
    const char *n = rig__extra_dup(name ? name : "?");
    if (!n) return NULL;            /* arena overflow already flagged */
    struct rig_extra_field *f = &g_extra[g_extra_n++];
    f->name = n;
    return f;
}

/* Replay one buffered field to the (now-open) response via jcon. g_extra_active
 * is false here, so the rig_emit_* calls below hit jcon, not the buffer. */
static void rig__extra_replay_one(const struct rig_extra_field *f) {
    switch (f->type) {
        case EX_I64:  rig_emit_i64 (f->name, f->i); break;
        case EX_U64:  rig_emit_u64 (f->name, f->u); break;
        case EX_BOOL: rig_emit_bool(f->name, f->i != 0); break;
        case EX_DBL:  rig_emit_double(f->name, f->d); break;
        case EX_STR:  rig_emit_str (f->name, f->s); break;
        case EX_NULL:
        default:      rig_emit_null(f->name); break;
    }
}

/* Reset extra-fields state to empty/inactive. Runs once per command (top of
 * rig_dispatch) so buffered fields from a previous command — e.g. one that
 * longjmp'd out of the trampoline via RIG_ASSERT, skipping rig__io_extra_end() —
 * cannot leak into the next command's response. */
void rig_io_extra_reset(void) {
    g_extra_n         = 0;
    g_extra_arena_len = 0;
    g_extra_active    = false;
    g_extra_in_body   = false;
    g_extra_used      = false;
    g_extra_truncated = false;
}

/* Called by the trampoline before invoking the user fn body. */
void rig__io_extra_begin(void) {
    g_extra_n         = 0;
    g_extra_arena_len = 0;
    g_extra_truncated = false;
    g_extra_active    = true;
    g_extra_in_body   = true;
    g_extra_used      = true;
}

/* Called by the trampoline after the user fn body returns normally. Buffered
 * fields stay until rig_io_resp_begin() replays them. */
void rig__io_extra_end(void) { g_extra_active = false; g_extra_in_body = false; }

void rig_io_line_begin(void) {
    /* If a fn body is mid-flight and is about to emit a full JSON line (event,
     * log, assert), suspend buffering so the line's rig_emit_* calls go to jcon
     * (the transport); rig_io_line_end() re-arms it if still in a body. */
    g_extra_active = false;
    rig__emit_sentinel();
    jcon_start(RIG_MINIFY, rig__jcon_putc, NULL);
}
void rig_io_line_end(void) {
    (void)jcon_end();
    /* jcon_end() only emits a trailing '\n' in pretty-print (non-minified) mode.
     * In minified mode we must append it ourselves. */
    if (RIG_MINIFY) (void)rig_putc('\n');
    /* Re-arm extra-fields buffering if a fn body is still running (this line was
     * an event/log it emitted mid-body). */
    g_extra_active = g_extra_in_body;
}

void rig_io_resp_begin(const char *cmd) {
    rig_io_line_begin();
    rig_emit_str("cmd", cmd ? cmd : "?");
    /* Replay any fields buffered during the fn body, between "cmd" and "ret".
     * Only when capture was actually used this command (a hand-rolled built-in
     * that calls rig_io_resp_begin() directly never armed it). If the buffer
     * overflowed, emit "_emit_overflow":true instead of a partial field set.
     * g_extra_active is false here (rig__io_extra_end / rig_io_line_begin both
     * cleared it), so the rig_emit_* calls below go to jcon, not back into the
     * buffer. */
    if (g_extra_used) {
        if (g_extra_truncated) rig_emit_bool("_emit_overflow", true);
        else for (int i = 0; i < g_extra_n; i++) rig__extra_replay_one(&g_extra[i]);
        g_extra_n    = 0;
        g_extra_used = false;
    }
}
void rig_io_ret_void(void) { jcon_add_null("ret"); }

void rig_emit_i64 (const char *name, int64_t  v) {
    if (g_extra_active) { struct rig_extra_field *f = rig__extra_new(name); if (f) { f->type = EX_I64; f->i = v; } return; }
    jcon_add_int64(name, v);
}
void rig_emit_u64 (const char *name, uint64_t v) {
    if (g_extra_active) { struct rig_extra_field *f = rig__extra_new(name); if (f) { f->type = EX_U64; f->u = v; } return; }
    jcon_add_uint64(name, v);
}
void rig_emit_bool (const char *name, bool v) {
    if (g_extra_active) { struct rig_extra_field *f = rig__extra_new(name); if (f) { f->type = EX_BOOL; f->i = v ? 1 : 0; } return; }
    jcon_add_bool(name, v);
}
void rig_emit_null (const char *name) {
    if (g_extra_active) { struct rig_extra_field *f = rig__extra_new(name); if (f) { f->type = EX_NULL; } return; }
    jcon_add_null(name);
}
void rig_emit_double (const char *name, double v) {
    /* Non-finite → null: mirror the host's finite-only rule. jcon's %g would emit
     * bare inf/nan (invalid JSON); rig_emit_null handles both the capture and the
     * direct path, so guarding here covers replay too. (Also dodges the UB of
     * casting a non-finite double to int64_t in the no-JCON_ENABLE_FLOAT branch.) */
    if (!isfinite(v)) { rig_emit_null(name); return; }
    if (g_extra_active) { struct rig_extra_field *f = rig__extra_new(name); if (f) { f->type = EX_DBL; f->d = v; } return; }
#ifdef JCON_ENABLE_FLOAT
    jcon_add_double(name, v);
#else
    /* float support not compiled into jcon: degrade to integer part */
    jcon_add_int64(name, (int64_t)v);
#endif
}

/* JSON-escape `s` into a bounded scratch buffer, then hand it to jcon (which
 * does no escaping). Truncates with "..." if it would overflow. */
void rig_emit_str(const char *name, const char *s) {
    static char scratch[RIG_SCRATCH_SIZE];
    if (g_extra_active) {
        struct rig_extra_field *f = rig__extra_new(name);
        if (f) {
            if (!s) { f->type = EX_NULL; }
            else {
                const char *cp = rig__extra_dup(s);
                if (cp) { f->type = EX_STR; f->s = cp; }
                else    { g_extra_n--; }   /* roll back: arena overflowed on the value */
            }
        }
        return;
    }
    if (!s) { jcon_add_null(name); return; }
    size_t w = 0;
    const size_t cap = sizeof scratch - 1;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char tmp[8]; const char *seg; size_t seglen;
        switch (*p) {
            case '"':  seg = "\\\""; seglen = 2; break;
            case '\\': seg = "\\\\"; seglen = 2; break;
            case '\n': seg = "\\n";  seglen = 2; break;
            case '\r': seg = "\\r";  seglen = 2; break;
            case '\t': seg = "\\t";  seglen = 2; break;
            case '\b': seg = "\\b";  seglen = 2; break;
            case '\f': seg = "\\f";  seglen = 2; break;
            default:
                if (*p < 0x20 || *p == 0x7f) {
                    /* control char → "\u00XX"; hand-rolled to keep snprintf (and
                     * its format-string machinery) off this per-byte path. */
                    static const char hexd[] = "0123456789abcdef";
                    tmp[0] = '\\'; tmp[1] = 'u'; tmp[2] = '0'; tmp[3] = '0';
                    tmp[4] = hexd[*p >> 4]; tmp[5] = hexd[*p & 0x0f]; tmp[6] = '\0';
                    seg = tmp; seglen = 6;
                } else { tmp[0] = (char)*p; tmp[1] = '\0'; seg = tmp; seglen = 1; }
        }
        if (w + seglen > cap) {                 /* would overflow → truncate */
            if (w + 3 <= cap) { memcpy(scratch + w, "...", 3); w += 3; }
            break;
        }
        memcpy(scratch + w, seg, seglen); w += seglen;
    }
    scratch[w] = '\0';
    jcon_add_string(name, scratch);
}

/* ---- error envelopes -------------------------------------------------- */

static void rig__err_open(const char *cmd) {
    rig_io_line_begin();
    rig_emit_str("cmd", cmd ? cmd : "?");
    jcon_object_start("error");
}
static void rig__err_close(void) { jcon_object_end(); rig_io_line_end(); }

void rig_io_err_unknown_cmd(const char *cmd) {
    rig__err_open(cmd); rig_emit_str("code", "unknown_cmd"); rig__err_close();
}
void rig_io_err_argcount(const char *cmd, int expected, int got) {
    rig__err_open(cmd); rig_emit_str("code", "arg_count");
    rig_emit_i64("expected", expected); rig_emit_i64("got", got); rig__err_close();
}
void rig_io_err_badarg(const char *cmd, int arg, const char *expected_type) {
    rig__err_open(cmd); rig_emit_str("code", "bad_args");
    rig_emit_i64("arg", arg); rig_emit_str("expected", expected_type); rig__err_close();
}
void rig_io_err_overflow(const char *cmd) {
    rig__err_open(cmd); rig_emit_str("code", "bad_args"); rig_emit_str("msg", "line too long"); rig__err_close();
}
void rig_io_err_syntax(const char *cmd, const char *msg) {
    rig__err_open(cmd); rig_emit_str("code", "bad_args"); rig_emit_str("msg", msg ? msg : "syntax error"); rig__err_close();
}
void rig_io_err_assert(const char *cmd, const char *file, int line, const char *expr) {
    rig__err_open(cmd); rig_emit_str("code", "assert");
    rig_emit_str("file", file); rig_emit_i64("line", line); rig_emit_str("msg", expr); rig__err_close();
}
void rig_io_err_internal(const char *cmd, const char *msg) {
    rig__err_open(cmd); rig_emit_str("code", "internal"); rig_emit_str("msg", msg); rig__err_close();
}

/* ---- events & log ----------------------------------------------------- */

void rig_io_event_begin(const char *name) {
    rig_io_line_begin();
    rig_emit_str("event", name ? name : "?");
}
void rig_io_event_end(void) { rig_io_line_end(); }

void rig_log(const char *fmt, ...) {
#if defined(CONFIG_RIGLINK) && !defined(CONFIG_RIGLINK_LOG)
    (void)fmt; return;            /* compiled out */
#else
    char buf[RIG_SCRATCH_SIZE];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rig_io_line_begin();
    rig_emit_str("log", buf);
    rig_io_line_end();
#endif
}

/* ---- ISR-deferred event ring ------------------------------------------ */

static struct rig_evt_rec g_evt_ring[RIG_EVENT_QUEUE_DEPTH];
static int  g_evt_head = 0;     /* next slot to write   */
static int  g_evt_tail = 0;     /* next slot to read    */
static int  g_evt_count = 0;    /* records currently queued */
static int  g_evt_dropped = 0;  /* dropped since last flush */

void rig_io_event_enqueue(const struct rig_evt_rec *rec) {
    rig_irq_key_t k = RIG_IRQ_LOCK();
    if (g_evt_count == RIG_EVENT_QUEUE_DEPTH) {     /* full: drop oldest */
        g_evt_tail = (g_evt_tail + 1) % RIG_EVENT_QUEUE_DEPTH;
        g_evt_count--;
        if (g_evt_dropped < INT32_MAX) g_evt_dropped++;
    }
    g_evt_ring[g_evt_head] = *rec;
    g_evt_head = (g_evt_head + 1) % RIG_EVENT_QUEUE_DEPTH;
    g_evt_count++;
    RIG_IRQ_UNLOCK(k);
}

static void rig__emit_evt_pair(const struct rig_evt_pair *p) {
    switch (p->type) {
        case RIG_EVT_U64:  rig_emit_u64 (p->key, p->u); break;
        case RIG_EVT_BOOL: rig_emit_bool(p->key, p->i != 0); break;
        case RIG_EVT_I64:
        default:           rig_emit_i64 (p->key, p->i); break;
    }
}

void rig_io_event_flush(void) {
    for (;;) {
        struct rig_evt_rec rec;
        bool have = false;
        rig_irq_key_t k = RIG_IRQ_LOCK();
        if (g_evt_count > 0) { rec = g_evt_ring[g_evt_tail];
                               g_evt_tail = (g_evt_tail + 1) % RIG_EVENT_QUEUE_DEPTH;
                               g_evt_count--; have = true; }
        int dropped = 0;
        if (have && g_evt_dropped) { dropped = g_evt_dropped; g_evt_dropped = 0; }
        RIG_IRQ_UNLOCK(k);
        if (!have) break;
        rig_io_event_begin(rec.name);
        if (dropped) rig_emit_i64("_dropped", dropped);
        for (int i = 0; i < rec.npairs && i < RIG_EVT_MAX_PAIRS; i++) rig__emit_evt_pair(&rec.pairs[i]);
        rig_io_event_end();
    }
}
