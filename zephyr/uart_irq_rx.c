/* zephyr/uart_irq_rx.c — reference interrupt-driven UART shim for the poll
 * backend. Activated by CONFIG_RIGLINK_UART_IRQ_RX.
 *
 * Why this exists: the obvious hand-rolled poll-backend rig_getc,
 *
 *     int rig_getc(void) { unsigned char c;
 *         return uart_poll_in(rig_uart, &c) == 0 ? (int)c : -1; }
 *
 * DROPS BYTES on real hardware. uart_poll_in only ever returns whatever is in
 * the UART's RX holding register at the instant it is called; the pump
 * (rig_run()) only polls between other application work, so at 115200 baud a
 * byte arrives roughly every ~87 us and the next one overwrites the previous in
 * the RX register long before the loop comes back around. The host then sees a
 * truncated command line and the call times out. This is a classic trap.
 *
 * This shim implements rig_putc / rig_getc for the application: it enables the
 * UART RX interrupt, pushes every received byte into a ring buffer from the ISR,
 * and rig_getc() pops from that ring. No bytes are lost as long as the ring is
 * drained faster than RIGLINK_UART_IRQ_RX_BUF_SIZE bytes accumulate (the pump
 * drains it every rig_run() call). The application keeps calling rig_init() /
 * rig_run() exactly as before — only the two shims change, so do NOT also
 * implement rig_putc / rig_getc yourself when this option is on.
 *
 * For most real-hardware setups the Zephyr SHELL backend
 * (CONFIG_RIGLINK_BACKEND_SHELL=y) is the recommended transport: it gives you
 * IRQ-driven RX, line assembly, and multiplexing with logs for free. Reach for
 * this shim when you want the bare poll backend (no shell subsystem) but still
 * need a UART link that survives non-trivial baud rates.
 *
 * Pick the UART via the `zephyr,riglink-uart` chosen node in your devicetree
 * (overlay):
 *
 *     / { chosen { zephyr,riglink-uart = &uart1; }; };
 *
 * If that chosen node is absent this file falls back to `zephyr,console`.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/ring_buffer.h>

#include <errno.h>

#include "riglink.h"

#if DT_HAS_CHOSEN(zephyr_riglink_uart)
#define RIG_UART_NODE DT_CHOSEN(zephyr_riglink_uart)
#else
#define RIG_UART_NODE DT_CHOSEN(zephyr_console)
#endif

BUILD_ASSERT(DT_NODE_EXISTS(RIG_UART_NODE),
	     "CONFIG_RIGLINK_UART_IRQ_RX: no UART node. Set the "
	     "`zephyr,riglink-uart` chosen node (or `zephyr,console`).");

static const struct device *const rig_uart = DEVICE_DT_GET(RIG_UART_NODE);

RING_BUF_DECLARE(rig_rx_ring, CONFIG_RIGLINK_UART_IRQ_RX_BUF_SIZE);

static void rig_uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	if (!uart_irq_update(dev)) {
		return;
	}

	while (uart_irq_rx_ready(dev)) {
		uint8_t buf[32];
		int n = uart_fifo_read(dev, buf, sizeof(buf));
		if (n <= 0) {
			break;
		}
		/* Best-effort: if the ring is full we drop the overflow bytes
		 * here rather than block in the ISR. Size
		 * RIGLINK_UART_IRQ_RX_BUF_SIZE for your longest command line
		 * plus pump latency to avoid this. */
		(void)ring_buf_put(&rig_rx_ring, buf, (uint32_t)n);
	}
}

int rig_putc(char c)
{
	uart_poll_out(rig_uart, (unsigned char)c);
	return 0;
}

int rig_getc(void)
{
	uint8_t c;
	return ring_buf_get(&rig_rx_ring, &c, 1) == 1 ? (int)c : -1;
}

__weak void rig_reset(void)
{
	sys_reboot(SYS_REBOOT_COLD);
}

static int rig_uart_irq_rx_init(void)
{
	if (!device_is_ready(rig_uart)) {
		return -ENODEV;
	}

	int rc = uart_irq_callback_user_data_set(rig_uart, rig_uart_isr, NULL);
	if (rc < 0) {
		/* Driver lacks the IRQ-driven API (e.g. built without
		 * CONFIG_UART_INTERRUPT_DRIVEN, or a polling-only driver). */
		return rc;
	}

	uart_irq_rx_enable(rig_uart);
	return 0;
}
SYS_INIT(rig_uart_irq_rx_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
