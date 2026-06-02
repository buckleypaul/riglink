/* src/riglink_proto.c — input pipeline: line assembly, tokenizer, token→scalar. */
#include "riglink_internal.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- line assembly ---------------------------------------------------- */

void rig_line_init(struct rig_line *l) { l->len = 0; l->buf[0] = '\0'; l->overflow = false; }
void rig_line_reset(struct rig_line *l) { rig_line_init(l); }

int rig_line_feed(struct rig_line *l, char c) {
    /* l->len is incremented only while l->len < RIG_LINE_BUF_SIZE - 1, so it's
     * always a valid index here (<= RIG_LINE_BUF_SIZE - 1) — no clamp needed. */
    if (c == '\n') { l->buf[l->len] = '\0'; return 1; }
    if (c == '\r') return 0;                 /* ignore CR */
    if (l->len < RIG_LINE_BUF_SIZE - 1) { l->buf[l->len++] = c; }
    else { l->overflow = true; }             /* keep eating until '\n' */
    return 0;
}

/* ---- tokenizer -------------------------------------------------------- */

static bool rig__is_ws(char c) { return c == ' ' || c == '\t'; }

int rig_tokenize(const char *line, struct rig_tokens *t) {
    t->argc = 0; t->name = NULL;
    /* copy into mutable storage. `line` always comes from a struct rig_line buf
     * of the same RIG_LINE_BUF_SIZE, so this clamp can't currently trigger; it's
     * kept as a guard in case a caller ever passes a longer string. */
    size_t n = strlen(line);
    if (n >= sizeof t->storage) n = sizeof t->storage - 1;
    memcpy(t->storage, line, n); t->storage[n] = '\0';

    char *p = t->storage;
    int   slot = 0;                              /* 0 = name slot */
    while (*p) {
        while (rig__is_ws(*p)) p++;
        if (*p == '\0') break;
        if (slot == 0 && *p == '#') { return 0; }       /* whole line is a comment */
        if (slot > RIG_MAX_ARGS) return -1;             /* name + > RIG_MAX_ARGS args */

        char *tok;
        if (*p == '"') {
            p++;
            tok = p;
            char *w = p;                          /* write cursor for unescaping */
            while (*p && *p != '"') {
                if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) { *w++ = p[1]; p += 2; }
                else { *w++ = *p++; }
            }
            if (*p != '"') {                      /* unterminated */
                if (slot >= 1) t->name = t->argv[0];
                return -1;
            }
            *w = '\0';
            p++;                                  /* past closing quote */
            /* require a separator or end after a quoted token */
            if (*p && !rig__is_ws(*p)) { if (slot >= 1) t->name = t->argv[0]; return -1; }
        } else {
            tok = p;
            while (*p && !rig__is_ws(*p)) p++;
            if (*p) { *p = '\0'; p++; }
        }
        t->argv[slot++] = tok;
    }
    if (slot == 0) { t->name = NULL; t->argc = 0; return 0; }   /* blank line */
    t->name = t->argv[0];
    t->argc = slot - 1;
    return 0;
}

/* ---- token → C scalar ------------------------------------------------- */

bool rig_parse_bool(const char *tok, bool *out) {
    if (strcmp(tok, "true") == 0)  { *out = true;  return true; }
    if (strcmp(tok, "false") == 0) { *out = false; return true; }
    return false;
}

bool rig_parse_i64(const char *tok, int64_t *out) {
    if (!tok || !*tok) return false;
    errno = 0;
    char *end = NULL;
    long long v = strtoll(tok, &end, 0);     /* base 0 → 0x.. hex, leading sign ok */
    if (errno == ERANGE || end == tok || *end != '\0') return false;
    *out = (int64_t)v;
    return true;
}

bool rig_parse_u64(const char *tok, uint64_t *out) {
    if (!tok || !*tok) return false;
    /* reject a leading '-' explicitly (strtoull would wrap it) */
    const char *p = tok; while (*p == ' ') p++;
    if (*p == '-') return false;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(tok, &end, 0);
    if (errno == ERANGE || end == tok || *end != '\0') return false;
    *out = (uint64_t)v;
    return true;
}

bool rig_parse_double(const char *tok, double *out) {
    if (!tok || !*tok) return false;
    errno = 0;
    char *end = NULL;
    double v = strtod(tok, &end);
    if (end == tok || *end != '\0') return false;
    *out = v;
    return true;
}

bool rig_parse_i_range(const char *tok, int64_t lo, int64_t hi, int64_t *out) {
    int64_t v;
    if (!rig_parse_i64(tok, &v)) return false;
    if (v < lo || v > hi) return false;
    *out = v;
    return true;
}

bool rig_parse_u_range(const char *tok, uint64_t hi, uint64_t *out) {
    uint64_t v;
    if (!rig_parse_u64(tok, &v)) return false;
    if (v > hi) return false;
    *out = v;
    return true;
}

bool rig_parse_str(const char *tok, char *buf, size_t bufsz) {
    if (!tok || bufsz == 0) { if (bufsz) buf[0] = '\0'; return false; }
    size_t n = strlen(tok);
    if (n >= bufsz) n = bufsz - 1;          /* truncate */
    memcpy(buf, tok, n); buf[n] = '\0';
    return true;
}

bool rig_str_arg_too_long(const char *tok, size_t bufsz, int *got) {
    /* A token of `bufsz` or more chars can't fit alongside the NUL terminator
     * in a char[bufsz] buffer, so rig_parse_str would truncate it. Report that
     * up-front (with the original length in *got) so the caller can raise a
     * clear arg_too_long error rather than dispatch a silently-clipped value. */
    if (!tok || bufsz == 0) { if (got) *got = 0; return false; }
    size_t n = strlen(tok);
    if (got) *got = (n <= (size_t)INT_MAX) ? (int)n : INT_MAX;   /* clamp the report */
    return n >= bufsz;
}
