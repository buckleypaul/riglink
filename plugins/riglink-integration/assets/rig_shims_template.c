/*
 * rig_shims_template.c — the three application shims riglink requires, plus a
 * minimal main loop and an example exposed surface. Copy into the target
 * project, then:
 *   1. point RIG_UART_NODE at the UART dedicated to the link,
 *   2. replace the example RIG_EXPOSE/RIG_FN block with the functions under
 *      test,
 *   3. delete this comment.
 *
 * This is the Zephyr UART variant (mirrors samples/echo). For standalone /
 * bare-metal C, keep the three shim signatures and replace their bodies with
 * direct register/HAL access; drop the Zephyr includes and main().
 */
#include <riglink.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/reboot.h>

/* On native_sim the link is the PTY uart1; on real boards use the chosen
 * console (or a dedicated UART node label). */
#if defined(CONFIG_BOARD_NATIVE_SIM)
#define RIG_UART_NODE DT_NODELABEL(uart1)
#else
#define RIG_UART_NODE DT_CHOSEN(zephyr_console)
#endif
static const struct device *const rig_uart = DEVICE_DT_GET(RIG_UART_NODE);

/* --- the three required shims --- */
int rig_putc(char c) { uart_poll_out(rig_uart, (unsigned char)c); return 0; }
int rig_getc(void) { unsigned char c; return uart_poll_in(rig_uart, &c) == 0 ? (int)c : -1; }
void rig_reset(void) { sys_reboot(SYS_REBOOT_COLD); }

/* --- exposed surface: replace with the functions/variables under test --- */
static int add(int a, int b) { return a + b; }
RIG_EXPOSE(int, add, int, int);

RIG_FN(int, safe_div, int, int) { RIG_ASSERT(arg1 != 0); return arg0 / arg1; }

static int32_t g_counter;
RIG_EXPOSE_VAR(int32_t, g_counter);

/* --- pump --- */
int main(void) {
    if (!device_is_ready(rig_uart)) return -1;
    rig_init();
    while (rig_run()) {
        k_msleep(1);  /* keep tight; rig_run() dispatches one command per call */
    }
    rig_deinit();
    return 0;
}
