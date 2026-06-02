/* samples/echo — a minimal riglink application: implements the shims, exposes a
 * few functions, and fires a periodic event from a timer ISR. */
#include <riglink.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/reboot.h>

/* The UART carrying the riglink link. On native_sim this is the PTY uart1; on
 * the nRF52 DK it is uart0 (the J-Link VCOM). Pick by board via devicetree. */
#if defined(CONFIG_BOARD_NATIVE_SIM)
#define RIG_UART_NODE DT_NODELABEL(uart1)
#else
#define RIG_UART_NODE DT_CHOSEN(zephyr_console)
#endif
static const struct device *const rig_uart = DEVICE_DT_GET(RIG_UART_NODE);

#if !defined(CONFIG_RIGLINK_UART_IRQ_RX)
/* Hand-rolled poll-backend shims. NOTE: uart_poll_in() drops RX bytes above
 * trivial baud rates on real hardware — fine for native_sim, but on a board set
 * CONFIG_RIGLINK_UART_IRQ_RX=y to get the library's interrupt-driven shim (or
 * use the shell backend). See ARCHITECTURE.md §2.10. */
int rig_putc(char c) { uart_poll_out(rig_uart, (unsigned char)c); return 0; }
int rig_getc(void) { unsigned char c; return uart_poll_in(rig_uart, &c) == 0 ? (int)c : -1; }
void rig_reset(void) { sys_reboot(SYS_REBOOT_COLD); }
#else
/* With CONFIG_RIGLINK_UART_IRQ_RX=y the library's zephyr/uart_irq_rx.c provides
 * rig_putc / rig_getc (interrupt-driven RX into a ring buffer), so this sample
 * must NOT define them. rig_reset is also provided there as a __weak
 * sys_reboot(SYS_REBOOT_COLD); define a strong rig_reset() here to override it
 * if your board needs custom reset behaviour. */
#endif /* !CONFIG_RIGLINK_UART_IRQ_RX */

/* --- exposed surface --- */
static int add(int a, int b) { return a + b; }
RIG_EXPOSE(int, add, int, int);

RIG_FN(int, scale, int, float) { return (int)(arg0 * arg1); }

RIG_FN(void, board_info) {
    rig_emit("board", CONFIG_BOARD);
    rig_emit("uptime_ms", (int64_t)k_uptime_get());
}

static int32_t g_counter;
RIG_EXPOSE_VAR(int32_t, g_counter);

RIG_FN(int, check_even, int) { RIG_ASSERT((arg0 % 2) == 0); return arg0 / 2; }

/* periodic event from a timer ISR -> exercises the ISR-deferred event ring */
static uint32_t g_tick;
static void tick_cb(struct k_timer *t) { (void)t; RIG_EMIT_EVENT_FROM_ISR("tick", "n", g_tick++); }
K_TIMER_DEFINE(g_ticker, tick_cb, NULL);

int main(void) {
    if (!device_is_ready(rig_uart)) return -1;
    rig_init();
    k_timer_start(&g_ticker, K_MSEC(500), K_MSEC(500));
    while (rig_run()) {
        k_msleep(1);
    }
    rig_deinit();
    return 0;
}
