/* riglink.h — host-driven firmware test harness: expose firmware functions
 * over a serial link; results emitted as sentinel-framed JSON via jcon. */
#ifndef RIGLINK_H
#define RIGLINK_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RIGLINK_VERSION_MAJOR  0
#define RIGLINK_VERSION_MINOR  1
#define RIGLINK_VERSION_PATCH  0
#define RIGLINK_VERSION_STRING "0.1.0"

#include "jcon.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- application shims (provided by the application / platform) --------- */
/* These three are NOT defined by riglink — the application (or test harness)
 * supplies them. Declared here so the implementation gets a signature check. */
int  rig_putc(char c);   /* write one byte to the transport; 0 = ok, nonzero = error */
int  rig_getc(void);     /* read one byte; returns -1 (non-blocking) when no byte is available right now */
void rig_reset(void);    /* application-defined device reset (rig.reset invokes it; typically reboots) */

/* ---- lifecycle --------------------------------------------------------- */
/* Initialize riglink. Returns 0 on success. Emits a {"event":"ready","version":"0.1.0"} line. */
int  rig_init(void);
/* Pump: flushes queued (ISR-deferred) events, then consumes available input
 * bytes; when a complete command line is assembled it is parsed and dispatched
 * (one command per call). Returns true while running, false after rig_deinit().
 * Non-blocking: returns promptly when rig_getc() reports no data. */
bool rig_run(void);
/* Stop the pump (subsequent rig_run() returns false). Does not touch the transport. */
void rig_deinit(void);

/* ---- token → C scalar ------------------------------------------------- */
/* Each returns true on success (and writes *out), false on malformed token or
 * out-of-range value. Integers accept optional leading sign and "0x" hex.
 * Called by macro-expanded application code (via the rig_parse_*_ wrappers in
 * riglink_pp.h), so declared in the public header. */
bool rig_parse_bool   (const char *tok, bool     *out);
bool rig_parse_i64    (const char *tok, int64_t  *out);   /* full int64 range  */
bool rig_parse_u64    (const char *tok, uint64_t *out);   /* full uint64 range */
bool rig_parse_double (const char *tok, double   *out);
bool rig_parse_i_range(const char *tok, int64_t lo, int64_t hi, int64_t *out);
bool rig_parse_u_range(const char *tok, uint64_t hi, uint64_t *out);
/* Copy `tok` into `buf`, truncating to `bufsz - 1`. Returns false only when
 * tok is NULL or bufsz is 0 (an over-long string is truncated, not an error). */
bool rig_parse_str    (const char *tok, char *buf, size_t bufsz);
/* True if `tok` would not fit in a char[bufsz] buffer (strlen(tok) >= bufsz),
 * i.e. rig_parse_str would truncate it. Stores strlen(tok) in *got (may be
 * NULL). Used by the RIG_DECL_str trampoline to raise arg_too_long instead of
 * dispatching a silently-truncated `str` argument. */
bool rig_str_arg_too_long(const char *tok, size_t bufsz, int *got);

/* ---- output (riglink_io.c) -------------------------------------------- */
/* Begin a sentinel-framed JSON line: emits RIG_SENTINEL then jcon_start(). */
void rig_io_line_begin(void);
/* End the current line: jcon_end() (+ a trailing '\n' in minified mode). */
void rig_io_line_end(void);

/* Begin a command response: line_begin() + "cmd":<cmd>.  After this the caller
 * emits any rig_emit_* fields and then exactly one "ret" field, then calls
 * rig_io_line_end(). */
void rig_io_resp_begin(const char *cmd);
/* Emit `"ret":null` (for void-returning commands). */
void rig_io_ret_void(void);

/* Field emitters usable inside a response or event (thin jcon wrappers, plus a
 * JSON-escaping string emitter — jcon itself does no escaping). The macro layer
 * exposes rig_emit(name, value) via _Generic; these are the typed primitives. */
void rig_emit_i64   (const char *name, int64_t  v);
void rig_emit_u64   (const char *name, uint64_t v);
void rig_emit_bool  (const char *name, bool     v);
void rig_emit_double (const char *name, double  v);
void rig_emit_str   (const char *name, const char *s);   /* JSON-escapes s */
void rig_emit_null  (const char *name);

/* ---- error envelopes -------------------------------------------------- */
/* Each emits a full line: {"cmd":<cmd or "?">,"error":{"code":...,...}}.
 * `cmd` may be NULL. */
void rig_io_err_unknown_cmd(const char *cmd);
void rig_io_err_argcount   (const char *cmd, int expected, int got);
void rig_io_err_badarg     (const char *cmd, int arg, const char *expected_type);
/* code "arg_too_long": a `str` argument's token (`got` chars) is longer than the
 * per-arg buffer can hold (`max` usable chars, i.e. RIG_STR_ARG_SIZE - 1).
 * Emitted instead of silently truncating the value. */
void rig_io_err_arg_too_long(const char *cmd, int arg, int got, int max);
void rig_io_err_overflow   (const char *cmd);          /* code "bad_args", msg "line too long" */
void rig_io_err_syntax     (const char *cmd, const char *msg);  /* code "bad_args", msg <msg> */
void rig_io_err_assert     (const char *cmd, const char *file, int line, const char *expr);
void rig_io_err_internal   (const char *cmd, const char *msg);

/* ---- events & log ----------------------------------------------------- */

/* Immediate event emission (thread context only — jcon is not reentrant). The
 * RIG_EMIT_EVENT macro wraps these: begin, emit pairs, end. */
void rig_io_event_begin(const char *name);   /* line_begin() + "event":<name> */
void rig_io_event_end(void);                  /* line_end() */

/* ---- ISR-deferred event ring ------------------------------------------ */
/* RIG_EMIT_EVENT_FROM_ISR(name, k0,v0, ...) enqueues a fixed-shape record:
 * an event name pointer (must outlive the flush — use a string literal) and up
 * to RIG_EVT_MAX_PAIRS integer key/value pairs (keys are string literals too).
 * rig_run() calls rig_io_event_flush() to drain and emit them. Type definitions
 * live here (not in riglink_internal.h) because RIG_EMIT_EVENT_FROM_ISR
 * references them. */
#define RIG_EVT_MAX_PAIRS 4

enum rig_evt_vtype { RIG_EVT_I64 = 0, RIG_EVT_U64 = 1, RIG_EVT_BOOL = 2 };

struct rig_evt_pair { const char *key; enum rig_evt_vtype type; union { int64_t i; uint64_t u; }; };
struct rig_evt_rec  { const char *name; struct rig_evt_pair pairs[RIG_EVT_MAX_PAIRS]; int npairs; };

/* Enqueue one record (IRQ-safe). On overflow drops the oldest and bumps an
 * internal dropped counter. */
void rig_io_event_enqueue(const struct rig_evt_rec *rec);

/* rig_log(): printf-style log emission over the riglink transport.
 * Emitted as {"log":"..."} (JSON-escaped). No-op when CONFIG_RIGLINK_LOG is
 * disabled. Defined in riglink_io.c. */
void rig_log(const char *fmt, ...);

/* ---- command registry -------------------------------------------------- */

/* A command's trampoline. `cmd` is the command name (a string literal); `argv`
 * points at the first ARGUMENT token (the command name is NOT included), `argc`
 * is the number of argument tokens. The trampoline is responsible for arity
 * checking, argument parsing, and emitting exactly one response or error line. */
typedef void (*rig_cmd_fn)(const char *cmd, int argc, char **argv);

/* Registry entry. The RIG_EXPOSE / RIG_FN / RIG_EXPOSE_VAR macros define one of
 * these as a static and register it via a constructor; hand-rolled commands fill
 * it in and call rig_register() (or use the constructor attribute themselves).
 * `argtypes` is an array of `nargs` type-name strings (for rig.list); pass
 * nargs == RIG_NARGS_VARIADIC and a single-element {"..."} array for a command
 * that accepts any arguments. `ret` is the return type name ("void", "int", ...). */
#define RIG_NARGS_VARIADIC 0xFF

struct rig_cmd {
    const char         *name;
    rig_cmd_fn          fn;
    const char         *ret;
    const char *const  *argtypes;
    uint8_t             nargs;
    struct rig_cmd     *next;       /* set by rig_register; do not touch */
    bool                registered; /* set by rig_register; initialise to false / 0 */
};

void rig_register(struct rig_cmd *cmd);     /* prepend to the registry (idempotent for a given node) */

/* Preprocessor machinery + keyword dictionaries + parse/emit wrappers. Placed
 * after the rig_parse_* / rig_emit_* / rig_register declarations because the
 * static-inline wrappers and the RIG_* macros reference them. */
#include "riglink_pp.h"

/* ====================================================================== */
/* User-facing RIG_* macros                                               */
/* ====================================================================== */

#include <setjmp.h>

/* setjmp target used by RIG_ASSERT to bail out of a RIG_FN body. File-scope,
 * single command in flight at a time. Defined in riglink_core.c. */
extern jmp_buf rig__assert_jmp;

/* Name of the command currently being dispatched; set by the trampoline before
 * calling the fn body. Used by RIG_ASSERT to name the command in the error
 * response. Defined in riglink_core.c. */
extern const char *rig__cmd_active;

/* Extra-field capture: buffer rig_emit calls made from inside a RIG_FN body (as
 * typed records, no jcon involved) so they can be injected into the response
 * after any events the fn emitted. rig__io_extra_begin() starts capture;
 * rig__io_extra_end() ends it. rig_io_resp_begin() replays the buffered fields
 * automatically. Defined in riglink_io.c. Called only by the trampoline macros
 * below. */
void rig__io_extra_begin(void);
void rig__io_extra_end(void);

/* rig_emit(name, value): add a top-level sibling field to the current response
 * or event line. Compile-time dispatch over the supported value types.
 * NOTE: C11 <stdbool.h> types `true`/`false` as `int`; pass a `bool` variable
 * or `(bool)expr` to get a JSON boolean rather than 0/1. */
#define rig_emit(name, value) _Generic((value),                       \
        bool:                 rig_emit_bool,                          \
        char:                 rig_emit_i64,                           \
        signed char:          rig_emit_i64,                           \
        short:                rig_emit_i64,                           \
        int:                  rig_emit_i64,                           \
        long:                 rig_emit_i64,                           \
        long long:            rig_emit_i64,                           \
        unsigned char:        rig_emit_u64,                           \
        unsigned short:       rig_emit_u64,                           \
        unsigned int:         rig_emit_u64,                           \
        unsigned long:        rig_emit_u64,                           \
        unsigned long long:   rig_emit_u64,                           \
        float:                rig_emit_double,                        \
        double:               rig_emit_double,                        \
        char *:               rig_emit_str,                           \
        const char *:         rig_emit_str)(name, value)

/* RIG_ASSERT(cond): only inside a RIG_FN body. On failure: emits a complete
 * error line {"cmd":...,"error":{"code":"assert",...}} and longjmps back to
 * the trampoline (which will NOT emit a ret). The fn body runs BEFORE the
 * response line is opened, so no jcon session is active here — use
 * rig_io_err_assert() which opens its own complete line. */
#define RIG_ASSERT(cond) do {                                          \
    if (!(cond)) {                                                     \
        rig_io_err_assert(rig__cmd_active, __FILE__, __LINE__, #cond); \
        longjmp(rig__assert_jmp, 1);                                   \
    }                                                                  \
} while (0)

/* --- the trampoline body shared by RIG_EXPOSE / RIG_FN ---
 * CALLEE is either the user function (RIG_EXPOSE) or rig__fnbody_##name
 * (RIG_FN). Dispatched on whether there are zero arguments: RIG__TRAMPOLINE_1
 * (zero args) / RIG__TRAMPOLINE_0 (>=1). RIG__ISEMPTYV_<n> maps 0 -> 1 (yes,
 * empty) and n>0 -> 0; see riglink_pp.h-style mini-table below. */
#define RIG__ISEMPTYV_0 1
#define RIG__ISEMPTYV_1 0
#define RIG__ISEMPTYV_2 0
#define RIG__ISEMPTYV_3 0
#define RIG__ISEMPTYV_4 0
#define RIG__ISEMPTYV_5 0
#define RIG__ISEMPTYV_6 0
#define RIG__ISEMPTYV_7 0
#define RIG__ISEMPTYV_8 0

#define RIG__ARGT_ONE(i, T)  #T,

#define RIG__TRAMPOLINE(name, R, CALLEE, ...)                                                       \
    RIG_PP_CAT(RIG__TRAMPOLINE_, RIG_PP_CAT(RIG__ISEMPTYV_, RIG_PP_NARGS(__VA_ARGS__)))(name, R, CALLEE, __VA_ARGS__)

/* zero-argument form: no argtypes array, NULL argtypes in the rig_cmd.
 *
 * Execution order:
 *  1. rig__io_extra_begin() — rig_emit calls in the fn body are buffered as
 *     typed records (not written to the transport) so events can be emitted first.
 *  2. fn body runs — rig_emit() fields are buffered; RIG_EMIT_EVENT emits a
 *     complete event line to the transport (rig_io_line_begin suspends buffering
 *     so the event's fields reach jcon; rig_io_line_end re-arms it).
 *  3. rig__io_extra_end() — stops the capture (fields stay buffered).
 *  4. rig_io_resp_begin(cmd) — opens the response line; replays buffered fields.
 *  5. ret emission + rig_io_line_end().
 *  If setjmp returns non-zero (RIG_ASSERT fired), the error line was already
 *  emitted by rig_io_err_assert() and no further output is produced. */
#define RIG__TRAMPOLINE_1(name, R, CALLEE, ...)                                                     \
    static void RIG_PP_CAT(rig__tramp_, name)(const char *cmd, int argc, char **argv) {             \
        (void)argv;                                                                                 \
        if (argc != 0) { rig_io_err_argcount(cmd, 0, argc); return; }                               \
        rig__cmd_active = cmd;                                                                      \
        rig__io_extra_begin();                                                                      \
        if (setjmp(rig__assert_jmp) == 0) {                                                         \
            RIG_PP_IF(RIG_PP_IS_VOID(R))(                                                           \
                CALLEE(); rig__io_extra_end(); rig_io_resp_begin(cmd); rig_io_ret_void();            \
            )(                                                                                      \
                R _r = CALLEE(); rig__io_extra_end(); rig_io_resp_begin(cmd); RIG_EMIT_##R("ret", _r); \
            )                                                                                       \
            rig_io_line_end();                                                                      \
        }                                                                                           \
    }                                                                                               \
    static struct rig_cmd RIG_PP_CAT(rig__cmd_, name) = { #name, RIG_PP_CAT(rig__tramp_, name), #R, NULL, 0, NULL, false }; \
    __attribute__((constructor)) static void RIG_PP_CAT(rig__reg_, name)(void) { rig_register(&RIG_PP_CAT(rig__cmd_, name)); }

/* >=1 argument form.  Same execution model as RIG__TRAMPOLINE_1. */
#define RIG__TRAMPOLINE_0(name, R, CALLEE, ...)                                                     \
    static const char *const RIG_PP_CAT(rig__argt_, name)[] = { RIG_PP_FE(RIG__ARGT_ONE, __VA_ARGS__) };  \
    static void RIG_PP_CAT(rig__tramp_, name)(const char *cmd, int argc, char **argv) {             \
        if (argc != (int)RIG_PP_NARGS(__VA_ARGS__)) { rig_io_err_argcount(cmd, (int)RIG_PP_NARGS(__VA_ARGS__), argc); return; } \
        RIG_PP_FE(RIG_PP__DECL_ONE, __VA_ARGS__)                                                    \
        rig__cmd_active = cmd;                                                                      \
        rig__io_extra_begin();                                                                      \
        if (setjmp(rig__assert_jmp) == 0) {                                                         \
            RIG_PP_IF(RIG_PP_IS_VOID(R))(                                                           \
                CALLEE(RIG_PP_ARGLIST(RIG_PP_NARGS(__VA_ARGS__))); rig__io_extra_end(); rig_io_resp_begin(cmd); rig_io_ret_void(); \
            )(                                                                                      \
                R _r = CALLEE(RIG_PP_ARGLIST(RIG_PP_NARGS(__VA_ARGS__))); rig__io_extra_end(); rig_io_resp_begin(cmd); RIG_EMIT_##R("ret", _r); \
            )                                                                                       \
            rig_io_line_end();                                                                      \
        }                                                                                           \
    }                                                                                               \
    static struct rig_cmd RIG_PP_CAT(rig__cmd_, name) = { #name, RIG_PP_CAT(rig__tramp_, name), #R, \
        RIG_PP_CAT(rig__argt_, name), (uint8_t)RIG_PP_NARGS(__VA_ARGS__), NULL, false };            \
    __attribute__((constructor)) static void RIG_PP_CAT(rig__reg_, name)(void) { rig_register(&RIG_PP_CAT(rig__cmd_, name)); }

/* RIG_EXPOSE(R, fn, T0, T1, ...) — wrap an existing function. The trailing
 * `struct ...` forward declaration is just a slot for the caller's `;`. */
#define RIG_EXPOSE(R, fn, ...)  RIG__TRAMPOLINE(fn, R, fn, __VA_ARGS__) struct RIG_PP_CAT(rig__semi_, fn)

/* RIG_FN(R, name, T0, ...) { body } — define a custom command; args arrive as
 * arg0, arg1, ...; `return` a value of type R (or nothing for void). */
#define RIG_FN(R, name, ...)                                                                        \
    static R RIG_PP_CAT(rig__fnbody_, name)(RIG__FN_PARAMS(__VA_ARGS__));                            \
    RIG__TRAMPOLINE(name, R, RIG_PP_CAT(rig__fnbody_, name), __VA_ARGS__)                            \
    static R RIG_PP_CAT(rig__fnbody_, name)(RIG__FN_PARAMS(__VA_ARGS__))
/* parameter list: "(void)" when no args, "(T0 arg0, T1 arg1, ...)" otherwise */
#define RIG__FN_PARAMS(...)  RIG_PP_IF(RIG_PP_CAT(RIG__ISEMPTYV_, RIG_PP_NARGS(__VA_ARGS__)))(void)(RIG__FN_PARAMS_NE(__VA_ARGS__))
#define RIG__FN_PARAMS_NE(...)  RIG_PP_FE(RIG__FN_PARAM_ONE, __VA_ARGS__)
#define RIG__FN_PARAM_ONE(i, T)  RIG_PP_IF(RIG_PP_CAT(RIG__ISEMPTYV_, i))(RIG_CTYPE_##T arg0)(, RIG_CTYPE_##T RIG_PP_CAT(arg, i))
/* (i==0 -> "T0 arg0"; i>0 -> ", Ti argi") */

/* RIG_EXPOSE_VAR(T, var) — generate var.get / var.set <T value>. T must be a
 * scalar keyword; RIG_EXPOSE_VAR(str, ...) is a compile error (see RIG_VARSET_str). */
#define RIG_EXPOSE_VAR(T, var)                                                                      \
    static void RIG_PP_CAT(rig__varget_, var)(const char *cmd, int argc, char **argv) {             \
        (void)argv; if (argc != 0) { rig_io_err_argcount(cmd, 0, argc); return; }                   \
        rig_io_resp_begin(cmd); RIG_EMIT_##T("ret", var); rig_io_line_end(); }                      \
    static void RIG_PP_CAT(rig__varset_, var)(const char *cmd, int argc, char **argv) {             \
        if (argc != 1) { rig_io_err_argcount(cmd, 1, argc); return; }                               \
        RIG_VARSET_##T(var, cmd)                                                                    \
        rig_io_resp_begin(cmd); rig_io_ret_void(); rig_io_line_end(); }                              \
    static const char *const RIG_PP_CAT(rig__vart_, var)[] = { #T };                                \
    static struct rig_cmd RIG_PP_CAT(rig__cmd_varget_, var) = { #var ".get", RIG_PP_CAT(rig__varget_, var), #T, NULL, 0, NULL, false }; \
    static struct rig_cmd RIG_PP_CAT(rig__cmd_varset_, var) = { #var ".set", RIG_PP_CAT(rig__varset_, var), "void", RIG_PP_CAT(rig__vart_, var), 1, NULL, false }; \
    __attribute__((constructor)) static void RIG_PP_CAT(rig__reg_var_, var)(void) {                 \
        rig_register(&RIG_PP_CAT(rig__cmd_varget_, var)); rig_register(&RIG_PP_CAT(rig__cmd_varset_, var)); } \
    struct RIG_PP_CAT(rig__semi_var_, var)

/* RIG_EMIT_EVENT("name", "k0", v0, "k1", v1, ...) — immediate (thread context). */
#define RIG_EMIT_EVENT(evname, ...) do {                                                            \
    rig_io_event_begin(evname);                                                                     \
    RIG__EVT_PAIRS(__VA_ARGS__)                                                                     \
    rig_io_event_end();                                                                             \
} while (0)
/* pairs come in as k0,v0,k1,v1,...; emit rig_emit(k,v) for each pair. Fixed
 * unrolled form for up to 4 pairs (8 varargs). */
#define RIG__EVT_PAIRS(...)  RIG_PP_CAT(RIG__EVT_PAIRS_, RIG_PP_NARG(__VA_ARGS__))(__VA_ARGS__)
#define RIG__EVT_PAIRS_0()
#define RIG__EVT_PAIRS_2(k0,v0)                   rig_emit(k0,v0);
#define RIG__EVT_PAIRS_4(k0,v0,k1,v1)             rig_emit(k0,v0); rig_emit(k1,v1);
#define RIG__EVT_PAIRS_6(k0,v0,k1,v1,k2,v2)       rig_emit(k0,v0); rig_emit(k1,v1); rig_emit(k2,v2);
#define RIG__EVT_PAIRS_8(k0,v0,k1,v1,k2,v2,k3,v3) rig_emit(k0,v0); rig_emit(k1,v1); rig_emit(k2,v2); rig_emit(k3,v3);

/* RIG_EMIT_EVENT_FROM_ISR("name", "k0", v0, ...) — enqueue (ISR-safe). Up to
 * RIG_EVT_MAX_PAIRS integer pairs; values are taken as int64_t. */
#define RIG_EMIT_EVENT_FROM_ISR(evname, ...) do {                                                   \
    struct rig_evt_rec _rec = { .name = (evname), .npairs = 0 };                                    \
    RIG__EVT_ENQ_PAIRS(_rec, __VA_ARGS__)                                                           \
    rig_io_event_enqueue(&_rec);                                                                    \
} while (0)
#define RIG__EVT_ENQ_PAIRS(rec, ...) RIG_PP_CAT(RIG__EVT_ENQ_, RIG_PP_NARG(__VA_ARGS__))(rec, __VA_ARGS__)
#define RIG__EVT_ENQ_1PAIR(rec,k,v) (rec).pairs[(rec).npairs++] = (struct rig_evt_pair){ .key = (k), .type = RIG_EVT_I64, .i = (int64_t)(v) };
#define RIG__EVT_ENQ_0(rec)
#define RIG__EVT_ENQ_2(rec,k0,v0)                   RIG__EVT_ENQ_1PAIR(rec,k0,v0)
#define RIG__EVT_ENQ_4(rec,k0,v0,k1,v1)             RIG__EVT_ENQ_2(rec,k0,v0) RIG__EVT_ENQ_1PAIR(rec,k1,v1)
#define RIG__EVT_ENQ_6(rec,k0,v0,k1,v1,k2,v2)       RIG__EVT_ENQ_4(rec,k0,v0,k1,v1) RIG__EVT_ENQ_1PAIR(rec,k2,v2)
#define RIG__EVT_ENQ_8(rec,k0,v0,k1,v1,k2,v2,k3,v3) RIG__EVT_ENQ_6(rec,k0,v0,k1,v1,k2,v2) RIG__EVT_ENQ_1PAIR(rec,k3,v3)

#ifdef __cplusplus
}
#endif

#endif /* RIGLINK_H */
