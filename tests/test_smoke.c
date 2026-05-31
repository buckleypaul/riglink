#include "riglink_test.h"
#include "riglink.h"

RIG_TEST(version_string_is_defined) {
    RIG_CHECK_STR_EQ(RIGLINK_VERSION_STRING, "0.1.0");
}

RIG_TEST(mock_putc_captures_output) {
    RIG_CHECK_INT_EQ(rig_putc('A'), 0);
    RIG_CHECK_INT_EQ(rig_putc('B'), 0);
    RIG_CHECK_STR_EQ(rig_mock_out(), "AB");
}

RIG_TEST(mock_getc_replays_fed_bytes) {
    rig_mock_feed("hi\n");
    RIG_CHECK_INT_EQ(rig_getc(), 'h');
    RIG_CHECK_INT_EQ(rig_getc(), 'i');
    RIG_CHECK_INT_EQ(rig_getc(), '\n');
    RIG_CHECK_INT_EQ(rig_getc(), -1);
}

RIG_TEST_MAIN()
