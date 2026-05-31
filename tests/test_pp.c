#include "riglink_test.h"
#include "riglink.h"          /* pulls in riglink_pp.h */

RIG_TEST(pp_narg) {
    RIG_CHECK_INT_EQ(RIG_PP_NARG(), 0);
    RIG_CHECK_INT_EQ(RIG_PP_NARG(a), 1);
    RIG_CHECK_INT_EQ(RIG_PP_NARG(a,b,c), 3);
}
RIG_TEST(pp_is_void) {
    RIG_CHECK_INT_EQ(RIG_PP_IS_VOID(void), 1);
    RIG_CHECK_INT_EQ(RIG_PP_IS_VOID(int), 0);
    RIG_CHECK_INT_EQ(RIG_PP_IS_VOID(x_void), 0);
}
RIG_TEST(pp_nargs_handles_void) {
    RIG_CHECK_INT_EQ(RIG_PP_NARGS(), 0);
    RIG_CHECK_INT_EQ(RIG_PP_NARGS(void), 0);
    RIG_CHECK_INT_EQ(RIG_PP_NARGS(int, float), 2);
}
RIG_TEST(pp_if) {
    RIG_CHECK_INT_EQ(RIG_PP_IF(1)(10)(20), 10);
    RIG_CHECK_INT_EQ(RIG_PP_IF(0)(10)(20), 20);
}
RIG_TEST(pp_parse_wrappers) {
    int8_t b; RIG_CHECK(rig_parse_int8_t_("127", &b) && b == 127);
    RIG_CHECK(!rig_parse_int8_t_("128", &b));
    uint8_t u; RIG_CHECK(rig_parse_uint8_t_("255", &u) && u == 255);
    RIG_CHECK(!rig_parse_uint8_t_("256", &u));
    char s[8]; RIG_CHECK(rig_parse_str("hi", s, sizeof s)); RIG_CHECK_STR_EQ(s, "hi");
}

RIG_TEST_MAIN()
