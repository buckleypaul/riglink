/* tests/riglink_test.h — native test harness for riglink.
 *
 * Provides:
 *   - a mock for the application shims rig_putc / rig_getc / rig_reset, backed
 *     by struct rig_mock (output capture buffer + scripted input bytes);
 *   - assertion macros that count pass/fail;
 *   - RIG_TEST_MAIN() which runs every registered test and returns nonzero on
 *     any failure.
 *
 * Each test file: #include "riglink_test.h", define tests with RIG_TEST(name)
 * { ... }, and call RIG_TEST_MAIN() in main.
 */
#ifndef RIGLINK_TEST_H
#define RIGLINK_TEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- mock application shims -------------------------------------------- */

#define RIG_MOCK_OUT_CAP 8192
#define RIG_MOCK_IN_CAP  8192

struct rig_mock {
    char    out[RIG_MOCK_OUT_CAP];
    size_t  out_len;
    bool    out_fail;          /* if set, rig_putc returns -1 */
    char    in[RIG_MOCK_IN_CAP];
    size_t  in_len;
    size_t  in_pos;
    int     reset_count;
};

static struct rig_mock g_rig_mock;

static inline void rig_mock_reset(void) {
    memset(&g_rig_mock, 0, sizeof g_rig_mock);
}

/* Queue a NUL-terminated string of bytes for rig_getc to hand out. */
static inline void rig_mock_feed(const char *s) {
    size_t n = strlen(s);
    if (g_rig_mock.in_len + n > RIG_MOCK_IN_CAP) n = RIG_MOCK_IN_CAP - g_rig_mock.in_len;
    memcpy(g_rig_mock.in + g_rig_mock.in_len, s, n);
    g_rig_mock.in_len += n;
}

static inline const char *rig_mock_out(void) {
    g_rig_mock.out[g_rig_mock.out_len < RIG_MOCK_OUT_CAP ? g_rig_mock.out_len : RIG_MOCK_OUT_CAP - 1] = '\0';
    return g_rig_mock.out;
}

/* These ARE the application shims for the native build. */
int rig_putc(char c) {
    if (g_rig_mock.out_fail) return -1;
    if (g_rig_mock.out_len + 1 >= RIG_MOCK_OUT_CAP) return -1;
    g_rig_mock.out[g_rig_mock.out_len++] = c;
    return 0;
}
int rig_getc(void) {
    if (g_rig_mock.in_pos >= g_rig_mock.in_len) return -1;
    return (unsigned char)g_rig_mock.in[g_rig_mock.in_pos++];
}
void rig_reset(void) { g_rig_mock.reset_count++; }

/* ---- assertions & runner ---------------------------------------------- */

static int g_rig_tests_run = 0;
static int g_rig_tests_failed = 0;
static const char *g_rig_test_name = "?";
static bool g_rig_test_ok = true;

#define RIG_CHECK(cond) do {                                                  \
    if (!(cond)) {                                                            \
        g_rig_test_ok = false;                                                \
        fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
    }                                                                         \
} while (0)

#define RIG_CHECK_STR_EQ(actual, expected) do {                               \
    const char *_a = (actual), *_e = (expected);                              \
    if (strcmp(_a, _e) != 0) {                                                \
        g_rig_test_ok = false;                                                \
        fprintf(stderr, "  FAIL %s:%d:\n    expected: <%s>\n    actual:   <%s>\n", \
                __FILE__, __LINE__, _e, _a);                                  \
    }                                                                         \
} while (0)

#define RIG_CHECK_INT_EQ(actual, expected) do {                               \
    long long _a = (long long)(actual), _e = (long long)(expected);           \
    if (_a != _e) {                                                           \
        g_rig_test_ok = false;                                                \
        fprintf(stderr, "  FAIL %s:%d: expected %lld, got %lld\n",             \
                __FILE__, __LINE__, _e, _a);                                  \
    }                                                                         \
} while (0)

typedef void (*rig_test_fn)(void);
struct rig_test_entry { const char *name; rig_test_fn fn; };

/* Tests register themselves into a fixed table via a constructor. */
#ifndef RIG_TEST_MAX
#define RIG_TEST_MAX 256
#endif
static struct rig_test_entry g_rig_test_tbl[RIG_TEST_MAX];
static int g_rig_test_count = 0;

#define RIG_TEST(name)                                                        \
    static void name(void);                                                   \
    __attribute__((constructor)) static void rig__reg_##name(void) {          \
        if (g_rig_test_count < RIG_TEST_MAX)                                  \
            g_rig_test_tbl[g_rig_test_count++] = (struct rig_test_entry){ #name, name }; \
    }                                                                         \
    static void name(void)

#define RIG_TEST_MAIN()                                                       \
    int main(void) {                                                          \
        for (int i = 0; i < g_rig_test_count; i++) {                          \
            g_rig_test_name = g_rig_test_tbl[i].name;                         \
            g_rig_test_ok = true;                                             \
            rig_mock_reset();                                                 \
            g_rig_test_tbl[i].fn();                                           \
            g_rig_tests_run++;                                                \
            if (!g_rig_test_ok) { g_rig_tests_failed++;                       \
                fprintf(stderr, "[FAIL] %s\n", g_rig_test_name); }            \
        }                                                                     \
        fprintf(stderr, "%d run, %d failed\n", g_rig_tests_run, g_rig_tests_failed); \
        return g_rig_tests_failed ? 1 : 0;                                    \
    }

#endif /* RIGLINK_TEST_H */
