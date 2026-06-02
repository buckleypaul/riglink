/* riglink_pp.h — preprocessor machinery for the RIG_* macros. Included by
 * riglink.h (after the rig_parse_* / rig_emit_* declarations, which the
 * static-inline wrappers and the RIG_* macros below reference). Self-contained
 * apart from those: no Zephyr headers. */
#ifndef RIGLINK_PP_H
#define RIGLINK_PP_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- generic PP helpers ---------------------------------------------- */

#define RIG_PP_CAT(a, b)   RIG_PP_CAT_(a, b)
#define RIG_PP_CAT_(a, b)  a##b
#define RIG_PP_STR(x)      RIG_PP_STR_(x)
#define RIG_PP_STR_(x)     #x
#define RIG_PP_FIRST(a, ...) a

/* count 0..8 args (uses __VA_OPT__: GCC/Clang in gnu11+ support it). The
 * trailing comma after the sentinel list guarantees RIG_PP_NARG_'s `...`
 * always receives at least one (possibly empty) argument, which keeps
 * -Wvariadic-macro-arguments-omitted quiet for the zero-arg case. */
#define RIG_PP_NARG(...)   RIG_PP_NARG_(dummy __VA_OPT__(,) __VA_ARGS__, 8,7,6,5,4,3,2,1,0,)
#define RIG_PP_NARG_(d,_1,_2,_3,_4,_5,_6,_7,_8,N,...) N

/* is the token `void`? -> 1 / 0 */
#define RIG_PP_2ND(a, b, ...) b
#define RIG_PP_CHECK(...)     RIG_PP_2ND(__VA_ARGS__, 0,)
#define RIG_PP_PROBE(x)       x, 1,
#define RIG_PP_IS_VOID(x)     RIG_PP_CHECK(RIG_PP_CAT(RIG_PP__ISVOID_, x))
#define RIG_PP__ISVOID_void   RIG_PP_PROBE(~)

/* IF: RIG_PP_IF(1)(then)(else) -> then ; RIG_PP_IF(0)(then)(else) -> else */
#define RIG_PP_IF(c)   RIG_PP_CAT(RIG_PP_IF_, c)
#define RIG_PP_IF_1(t) t RIG_PP_EAT
#define RIG_PP_IF_0(t) RIG_PP_ID
#define RIG_PP_EAT(...)
#define RIG_PP_ID(...) __VA_ARGS__

/* "real" arg count: 0 for empty list, 0 for a single `void`, else RIG_PP_NARG */
#define RIG_PP_NARGS(...)  RIG_PP_CAT(RIG_PP_NARGS_, RIG_PP_CAT(RIG_PP_NARG(__VA_ARGS__), RIG_PP_IS_VOID(RIG_PP_FIRST(__VA_ARGS__,))))
#define RIG_PP_NARGS_00 0
#define RIG_PP_NARGS_10 1
#define RIG_PP_NARGS_11 0
#define RIG_PP_NARGS_20 2
#define RIG_PP_NARGS_30 3
#define RIG_PP_NARGS_40 4
#define RIG_PP_NARGS_50 5
#define RIG_PP_NARGS_60 6
#define RIG_PP_NARGS_70 7
#define RIG_PP_NARGS_80 8

/* FOR_EACH_IDX: RIG_PP_FE(m, a, b) -> m(0,a) m(1,b) ; RIG_PP_FE(m) -> nothing */
#define RIG_PP_FE(m, ...)  RIG_PP_CAT(RIG_PP_FE_, RIG_PP_NARG(__VA_ARGS__))(m __VA_OPT__(,) __VA_ARGS__)
#define RIG_PP_FE_0(m)
#define RIG_PP_FE_1(m,a)              m(0,a)
#define RIG_PP_FE_2(m,a,b)            m(0,a) m(1,b)
#define RIG_PP_FE_3(m,a,b,c)          m(0,a) m(1,b) m(2,c)
#define RIG_PP_FE_4(m,a,b,c,d)        m(0,a) m(1,b) m(2,c) m(3,d)
#define RIG_PP_FE_5(m,a,b,c,d,e)      m(0,a) m(1,b) m(2,c) m(3,d) m(4,e)
#define RIG_PP_FE_6(m,a,b,c,d,e,f)    m(0,a) m(1,b) m(2,c) m(3,d) m(4,e) m(5,f)
#define RIG_PP_FE_7(m,a,b,c,d,e,f,g)  m(0,a) m(1,b) m(2,c) m(3,d) m(4,e) m(5,f) m(6,g)
#define RIG_PP_FE_8(m,a,b,c,d,e,f,g,h) m(0,a) m(1,b) m(2,c) m(3,d) m(4,e) m(5,f) m(6,g) m(7,h)

/* comma-separated _a0,_a1,...,_a(n-1) for building the call expression */
#define RIG_PP_ARGLIST(n)  RIG_PP_CAT(RIG_PP_ARGLIST_, n)
#define RIG_PP_ARGLIST_0
#define RIG_PP_ARGLIST_1 _a0
#define RIG_PP_ARGLIST_2 _a0,_a1
#define RIG_PP_ARGLIST_3 _a0,_a1,_a2
#define RIG_PP_ARGLIST_4 _a0,_a1,_a2,_a3
#define RIG_PP_ARGLIST_5 _a0,_a1,_a2,_a3,_a4
#define RIG_PP_ARGLIST_6 _a0,_a1,_a2,_a3,_a4,_a5
#define RIG_PP_ARGLIST_7 _a0,_a1,_a2,_a3,_a4,_a5,_a6
#define RIG_PP_ARGLIST_8 _a0,_a1,_a2,_a3,_a4,_a5,_a6,_a7

/* ---- keyword dictionaries -------------------------------------------- */
/* RIG_CTYPE_<kw> : C type. RIG_DECL_<kw>(i, cmd) : declare _a<i> and parse
 * argv[i] into it (bad_args + return on failure). RIG_EMIT_<kw>(name, v) :
 * emit value v under `name`. `str` is special-cased (a stack char buffer). */

#ifndef RIG_STR_ARG_SIZE
#define RIG_STR_ARG_SIZE 64        /* per-`str`-argument stack buffer; override before including */
#endif

/* parse wrappers (width-checked); names end in '_' to avoid colliding with rig_parse_* */
static inline bool rig_parse_int_     (const char*t,int*o)      {int64_t v; if(!rig_parse_i_range(t,INT_MIN,INT_MAX,&v))return false;*o=(int)v;return true;}
static inline bool rig_parse_unsigned_(const char*t,unsigned*o) {uint64_t v;if(!rig_parse_u_range(t,UINT_MAX,&v))return false;*o=(unsigned)v;return true;}
/* `char` arg: accept -128..255 — the union of the signed-char range (-128..127)
 * and the unsigned-char range (0..255) — so the same firmware works whether the
 * platform's `char` is signed or unsigned. Caveat: a value outside the platform's
 * actual `char` range is not an error here; it's cast to `char` and may therefore
 * differ in sign from what the host sent. */
static inline bool rig_parse_char_    (const char*t,char*o)     {int64_t v; if(!rig_parse_i_range(t,-128,255,&v))return false;*o=(char)v;return true;}
static inline bool rig_parse_int8_t_  (const char*t,int8_t*o)   {int64_t v; if(!rig_parse_i_range(t,INT8_MIN,INT8_MAX,&v))return false;*o=(int8_t)v;return true;}
static inline bool rig_parse_int16_t_ (const char*t,int16_t*o)  {int64_t v; if(!rig_parse_i_range(t,INT16_MIN,INT16_MAX,&v))return false;*o=(int16_t)v;return true;}
static inline bool rig_parse_int32_t_ (const char*t,int32_t*o)  {int64_t v; if(!rig_parse_i_range(t,INT32_MIN,INT32_MAX,&v))return false;*o=(int32_t)v;return true;}
static inline bool rig_parse_int64_t_ (const char*t,int64_t*o)  {return rig_parse_i64(t,o);}
static inline bool rig_parse_uint8_t_ (const char*t,uint8_t*o)  {uint64_t v;if(!rig_parse_u_range(t,UINT8_MAX,&v))return false;*o=(uint8_t)v;return true;}
static inline bool rig_parse_uint16_t_(const char*t,uint16_t*o) {uint64_t v;if(!rig_parse_u_range(t,UINT16_MAX,&v))return false;*o=(uint16_t)v;return true;}
static inline bool rig_parse_uint32_t_(const char*t,uint32_t*o) {uint64_t v;if(!rig_parse_u_range(t,UINT32_MAX,&v))return false;*o=(uint32_t)v;return true;}
static inline bool rig_parse_uint64_t_(const char*t,uint64_t*o) {return rig_parse_u64(t,o);}
static inline bool rig_parse_bool_    (const char*t,bool*o)     {return rig_parse_bool(t,o);}
static inline bool rig_parse_float_   (const char*t,float*o)    {double d; if(!rig_parse_double(t,&d))return false;*o=(float)d;return true;}
static inline bool rig_parse_double_  (const char*t,double*o)   {return rig_parse_double(t,o);}
static inline bool rig_parse_size_t_  (const char*t,size_t*o)   {uint64_t v;if(!rig_parse_u_range(t,(uint64_t)SIZE_MAX,&v))return false;*o=(size_t)v;return true;}
static inline bool rig_parse_uintptr_t_(const char*t,uintptr_t*o){uint64_t v;if(!rig_parse_u_range(t,(uint64_t)UINTPTR_MAX,&v))return false;*o=(uintptr_t)v;return true;}
static inline bool rig_parse_intptr_t_(const char*t,intptr_t*o) {int64_t v; if(!rig_parse_i_range(t,INTPTR_MIN,INTPTR_MAX,&v))return false;*o=(intptr_t)v;return true;}
static inline bool rig_parse_ptrdiff_t_(const char*t,ptrdiff_t*o){int64_t v; if(!rig_parse_i_range(t,PTRDIFF_MIN,PTRDIFF_MAX,&v))return false;*o=(ptrdiff_t)v;return true;}

#define RIG_CTYPE_int        int
#define RIG_CTYPE_unsigned   unsigned
#define RIG_CTYPE_char       char
#define RIG_CTYPE_bool       bool
#define RIG_CTYPE_int8_t     int8_t
#define RIG_CTYPE_int16_t    int16_t
#define RIG_CTYPE_int32_t    int32_t
#define RIG_CTYPE_int64_t    int64_t
#define RIG_CTYPE_uint8_t    uint8_t
#define RIG_CTYPE_uint16_t   uint16_t
#define RIG_CTYPE_uint32_t   uint32_t
#define RIG_CTYPE_uint64_t   uint64_t
#define RIG_CTYPE_size_t     size_t
#define RIG_CTYPE_uintptr_t  uintptr_t
#define RIG_CTYPE_intptr_t   intptr_t
#define RIG_CTYPE_ptrdiff_t  ptrdiff_t
#define RIG_CTYPE_float      float
#define RIG_CTYPE_double     double
#define RIG_CTYPE_str        const char *

/* RIG_DECL_<kw>(i, cmd): scalars share one helper; `str` is special. */
#define RIG_DECL_SCALAR(i, cmd, kw) \
    RIG_CTYPE_##kw RIG_PP_CAT(_a, i); \
    if (!RIG_PP_CAT(RIG_PP_CAT(rig_parse_, kw), _)(argv[i], &RIG_PP_CAT(_a, i))) { rig_io_err_badarg(cmd, i, #kw); return; }
#define RIG_DECL_int(i,cmd)       RIG_DECL_SCALAR(i,cmd,int)
#define RIG_DECL_unsigned(i,cmd)  RIG_DECL_SCALAR(i,cmd,unsigned)
#define RIG_DECL_char(i,cmd)      RIG_DECL_SCALAR(i,cmd,char)
#define RIG_DECL_bool(i,cmd)      RIG_DECL_SCALAR(i,cmd,bool)
#define RIG_DECL_int8_t(i,cmd)    RIG_DECL_SCALAR(i,cmd,int8_t)
#define RIG_DECL_int16_t(i,cmd)   RIG_DECL_SCALAR(i,cmd,int16_t)
#define RIG_DECL_int32_t(i,cmd)   RIG_DECL_SCALAR(i,cmd,int32_t)
#define RIG_DECL_int64_t(i,cmd)   RIG_DECL_SCALAR(i,cmd,int64_t)
#define RIG_DECL_uint8_t(i,cmd)   RIG_DECL_SCALAR(i,cmd,uint8_t)
#define RIG_DECL_uint16_t(i,cmd)  RIG_DECL_SCALAR(i,cmd,uint16_t)
#define RIG_DECL_uint32_t(i,cmd)  RIG_DECL_SCALAR(i,cmd,uint32_t)
#define RIG_DECL_uint64_t(i,cmd)  RIG_DECL_SCALAR(i,cmd,uint64_t)
#define RIG_DECL_size_t(i,cmd)    RIG_DECL_SCALAR(i,cmd,size_t)
#define RIG_DECL_uintptr_t(i,cmd) RIG_DECL_SCALAR(i,cmd,uintptr_t)
#define RIG_DECL_intptr_t(i,cmd)  RIG_DECL_SCALAR(i,cmd,intptr_t)
#define RIG_DECL_ptrdiff_t(i,cmd) RIG_DECL_SCALAR(i,cmd,ptrdiff_t)
#define RIG_DECL_float(i,cmd)     RIG_DECL_SCALAR(i,cmd,float)
#define RIG_DECL_double(i,cmd)    RIG_DECL_SCALAR(i,cmd,double)
/* A `str` arg longer than the buffer would be silently truncated by
 * rig_parse_str (e.g. a 64-char hex key clipped to 63 → odd length →
 * confusing downstream decode failure). Detect that here and emit a clear
 * arg_too_long error instead of dispatching with a corrupted value. */
#define RIG_DECL_str(i,cmd) \
    char RIG_PP_CAT(_a, i)[RIG_STR_ARG_SIZE]; \
    { int RIG_PP_CAT(_got, i); \
      if (rig_str_arg_too_long(argv[i], sizeof RIG_PP_CAT(_a, i), &RIG_PP_CAT(_got, i))) { \
          rig_io_err_arg_too_long(cmd, i, RIG_PP_CAT(_got, i), (int)(sizeof RIG_PP_CAT(_a, i)) - 1); return; } } \
    if (!rig_parse_str(argv[i], RIG_PP_CAT(_a, i), sizeof RIG_PP_CAT(_a, i))) { rig_io_err_badarg(cmd, i, "str"); return; }
/* applied per index by the trampoline: */
#define RIG_PP__DECL_ONE(i, T)  RIG_DECL_##T(i, cmd)

/* RIG_VARSET_<kw>(var, cmd): the body of a generated `<var>.set` trampoline —
 * parse argv[0] into the keyword's C type, then assign it to `var`. `str` is
 * deliberately rejected at compile time: RIG_DECL_str declares a *stack*
 * char[] buffer, so `var = <that buffer>` would store a pointer that dangles
 * the instant the trampoline returns. Apps that want a settable string must
 * wrap it by hand (e.g. RIG_FN with a strncpy into a static buffer). */
#define RIG_VARSET_SCALAR(var, cmd, kw)  RIG_DECL_##kw(0, cmd) (var) = _a0;
#define RIG_VARSET_int(var,cmd)       RIG_VARSET_SCALAR(var,cmd,int)
#define RIG_VARSET_unsigned(var,cmd)  RIG_VARSET_SCALAR(var,cmd,unsigned)
#define RIG_VARSET_char(var,cmd)      RIG_VARSET_SCALAR(var,cmd,char)
#define RIG_VARSET_bool(var,cmd)      RIG_VARSET_SCALAR(var,cmd,bool)
#define RIG_VARSET_int8_t(var,cmd)    RIG_VARSET_SCALAR(var,cmd,int8_t)
#define RIG_VARSET_int16_t(var,cmd)   RIG_VARSET_SCALAR(var,cmd,int16_t)
#define RIG_VARSET_int32_t(var,cmd)   RIG_VARSET_SCALAR(var,cmd,int32_t)
#define RIG_VARSET_int64_t(var,cmd)   RIG_VARSET_SCALAR(var,cmd,int64_t)
#define RIG_VARSET_uint8_t(var,cmd)   RIG_VARSET_SCALAR(var,cmd,uint8_t)
#define RIG_VARSET_uint16_t(var,cmd)  RIG_VARSET_SCALAR(var,cmd,uint16_t)
#define RIG_VARSET_uint32_t(var,cmd)  RIG_VARSET_SCALAR(var,cmd,uint32_t)
#define RIG_VARSET_uint64_t(var,cmd)  RIG_VARSET_SCALAR(var,cmd,uint64_t)
#define RIG_VARSET_size_t(var,cmd)    RIG_VARSET_SCALAR(var,cmd,size_t)
#define RIG_VARSET_uintptr_t(var,cmd) RIG_VARSET_SCALAR(var,cmd,uintptr_t)
#define RIG_VARSET_intptr_t(var,cmd)  RIG_VARSET_SCALAR(var,cmd,intptr_t)
#define RIG_VARSET_ptrdiff_t(var,cmd) RIG_VARSET_SCALAR(var,cmd,ptrdiff_t)
#define RIG_VARSET_float(var,cmd)     RIG_VARSET_SCALAR(var,cmd,float)
#define RIG_VARSET_double(var,cmd)    RIG_VARSET_SCALAR(var,cmd,double)
#define RIG_VARSET_str(var,cmd) \
    _Static_assert(0, "RIG_EXPOSE_VAR(str, ...) is not supported: a `str` setter " \
                      "would store a pointer to a stack buffer; wrap it by hand"); \
    (void)cmd;

/* RIG_EMIT_<kw>(name, v) */
#define RIG_EMIT_int(n,v)       rig_emit_i64((n),(int64_t)(v))
#define RIG_EMIT_unsigned(n,v)  rig_emit_u64((n),(uint64_t)(v))
#define RIG_EMIT_char(n,v)      rig_emit_i64((n),(int64_t)(v))
#define RIG_EMIT_bool(n,v)      rig_emit_bool((n),(v))
#define RIG_EMIT_int8_t(n,v)    rig_emit_i64((n),(int64_t)(v))
#define RIG_EMIT_int16_t(n,v)   rig_emit_i64((n),(int64_t)(v))
#define RIG_EMIT_int32_t(n,v)   rig_emit_i64((n),(int64_t)(v))
#define RIG_EMIT_int64_t(n,v)   rig_emit_i64((n),(int64_t)(v))
#define RIG_EMIT_uint8_t(n,v)   rig_emit_u64((n),(uint64_t)(v))
#define RIG_EMIT_uint16_t(n,v)  rig_emit_u64((n),(uint64_t)(v))
#define RIG_EMIT_uint32_t(n,v)  rig_emit_u64((n),(uint64_t)(v))
#define RIG_EMIT_uint64_t(n,v)  rig_emit_u64((n),(uint64_t)(v))
#define RIG_EMIT_size_t(n,v)    rig_emit_u64((n),(uint64_t)(v))
#define RIG_EMIT_uintptr_t(n,v) rig_emit_u64((n),(uint64_t)(v))
#define RIG_EMIT_intptr_t(n,v)  rig_emit_i64((n),(int64_t)(v))
#define RIG_EMIT_ptrdiff_t(n,v) rig_emit_i64((n),(int64_t)(v))
#define RIG_EMIT_float(n,v)     rig_emit_double((n),(double)(v))
#define RIG_EMIT_double(n,v)    rig_emit_double((n),(double)(v))
#define RIG_EMIT_str(n,v)       rig_emit_str((n),(v))

/* type-name string for rig.list metadata */
#define RIG_TNAME(T) #T

#endif /* RIGLINK_PP_H */
