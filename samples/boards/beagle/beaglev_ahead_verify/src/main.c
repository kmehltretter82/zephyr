/*
 * Copyright (c) 2026 BeagleV-Ahead Zephyr Port
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Comprehensive Hardware Verification Suite for Zephyr RTOS on
 * BeagleV-Ahead (T-Head TH1520 quad-core XuanTie C910).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/arch/riscv/csr.h>
#include <string.h>
#include <soc.h>

/* 1. SMP test threads */
#define SMP_WORKERS CONFIG_MP_MAX_NUM_CPUS
#define STACK_SIZE  2048

K_THREAD_STACK_ARRAY_DEFINE(smp_stacks, SMP_WORKERS, STACK_SIZE);
static struct k_thread smp_threads[SMP_WORKERS];
static K_SEM_DEFINE(smp_done_sem, 0, SMP_WORKERS);
static atomic_t cpu_seen_mask;

static void smp_worker_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* Spin briefly so all 4 harts pick up work concurrently */
	uint64_t start = k_cycle_get_64();

	while ((k_cycle_get_64() - start) < 15000ULL) {
		unsigned int cpu_id = arch_curr_cpu()->id;

		atomic_or(&cpu_seen_mask, BIT(cpu_id));
		k_yield();
	}

	k_sem_give(&smp_done_sem);
}

static int verify_cpu_and_smp(void)
{
	printk("\n[1/8] CPU, Vendor CSRs & %d-Hart SMP Verification\n", SMP_WORKERS);

#ifdef CONFIG_RISCV_S_MODE
	unsigned long sxstatus = csr_read(0x5c0);

	printk("  Mode: Supervisor (S-Mode), CSR_TH_SXSTATUS = 0x%016lx\n", sxstatus);
	if ((sxstatus & BIT(22)) == 0) {
		printk("  [FAIL] THEADISAEE bit not set in CSR_TH_SXSTATUS\n");
		return -1;
	}
#else
	unsigned long mxstatus = csr_read(0x7c0);
	unsigned long cpuid0 = csr_read(0xfc0);

	printk("  Mode: Machine (M-Mode), CSR_TH_MXSTATUS = 0x%016lx, CPUID0 = 0x%08lx\n",
	       mxstatus, cpuid0);
	if ((mxstatus & BIT(22)) == 0 || cpuid0 != 0x090c090dUL) {
		printk("  [FAIL] Unexpected C910 vendor CSRs\n");
		return -1;
	}
#endif

	uint32_t freq = sys_clock_hw_cycles_per_sec();
	uint64_t c0 = k_cycle_get_64();

	k_msleep(50);
	uint64_t elapsed = k_cycle_get_64() - c0;

	printk("  Timer freq: %u Hz, 50 ms sleep elapsed: %llu cycles\n",
	       freq, (unsigned long long)elapsed);
	if (freq != 3000000U || elapsed < 100000ULL) {
		printk("  [FAIL] System timer frequency or sleep cycle count out of range\n");
		return -1;
	}

	atomic_set(&cpu_seen_mask, BIT(arch_curr_cpu()->id));
	for (int i = 0; i < SMP_WORKERS; i++) {
		k_thread_create(&smp_threads[i], smp_stacks[i], STACK_SIZE,
				smp_worker_fn, NULL, NULL, NULL,
				K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
	}
	for (int i = 0; i < SMP_WORKERS; i++) {
		k_sem_take(&smp_done_sem, K_FOREVER);
	}

	atomic_val_t mask = atomic_get(&cpu_seen_mask);
	atomic_val_t expected = BIT_MASK(SMP_WORKERS);

	printk("  Active CPU bitmask: 0x%lx (expected 0x%lx)\n",
	       (unsigned long)mask, (unsigned long)expected);
	if ((mask & expected) != expected) {
		printk("  [FAIL] Not all %d CPUs executed worker threads\n", SMP_WORKERS);
		return -1;
	}

	printk("  [PASS] CPU, C910 CSRs, 3 MHz timer & %d-hart SMP verified\n", SMP_WORKERS);
	return 0;
}

/* 2. Interrupt-driven UART verification on mikrobus_serial (uart3, PLIC IRQ 39) */
static K_SEM_DEFINE(uart3_tx_sem, 0, 1);
static const char uart3_msg[] = "MIKROBUS_UART3_IRQ_OK";
static volatile size_t uart3_tx_idx;

static void uart3_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	for (;;) {
		uart_irq_update(dev);
		if (!uart_irq_is_pending(dev)) {
			break;
		}
		if (uart_irq_tx_ready(dev)) {
			if (uart3_tx_idx < sizeof(uart3_msg) - 1) {
				uint8_t c = (uint8_t)uart3_msg[uart3_tx_idx++];

				uart_fifo_fill(dev, &c, 1);
			} else {
				uart_irq_tx_disable(dev);
				k_sem_give(&uart3_tx_sem);
			}
		}
	}
}

static int verify_uart_irq(void)
{
	printk("\n[2/8] PLIC Interrupt-Driven UART Verification (mikrobus_serial / uart3)\n");
	const struct device *uart3 = DEVICE_DT_GET(DT_NODELABEL(mikrobus_serial));

	if (!device_is_ready(uart3)) {
		printk("  [FAIL] mikrobus_serial (uart3) not ready\n");
		return -1;
	}

	uart3_tx_idx = 0;
	uart_irq_callback_set(uart3, uart3_isr);
	uart_irq_tx_enable(uart3);

	if (k_sem_take(&uart3_tx_sem, K_MSEC(500)) != 0) {
		printk("  [FAIL] Timeout waiting for PLIC IRQ 39 on uart3\n");
		return -1;
	}

	printk("  Transmitted %zu bytes via PLIC IRQ 39 on uart3\n", uart3_tx_idx);
	printk("  [PASS] PLIC interrupt-driven UART3 verified\n");
	return 0;
}

/* 3. DesignWare GPIO & Onboard User LEDs (USR0..USR4 on gpio4 pins 8..12) + AP6203BM */
static const struct gpio_dt_spec usr_leds[] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led4), gpios),
};

static int verify_gpio_and_leds(void)
{
	printk("\n[3/8] DesignWare GPIO, Onboard User LEDs (USR0..USR4) & AP6203BM Verification\n");

	for (size_t i = 0; i < ARRAY_SIZE(usr_leds); i++) {
		if (!gpio_is_ready_dt(&usr_leds[i])) {
			printk("  [FAIL] LED %zu GPIO controller not ready\n", i);
			return -1;
		}
		int ret = gpio_pin_configure_dt(&usr_leds[i], GPIO_OUTPUT_INACTIVE);

		if (ret != 0) {
			printk("  [FAIL] Failed to configure LED %zu: %d\n", i, ret);
			return -1;
		}
	}

	/* Drive alternating pattern 0b10101 (0x15) on USR0..USR4 (gpio4 pins 8..12) */
	for (size_t i = 0; i < ARRAY_SIZE(usr_leds); i++) {
		gpio_pin_set_dt(&usr_leds[i], (i % 2 == 0) ? 1 : 0);
	}

	uint32_t gpio4_dr = (sys_read32(0xfffff52000UL) >> 8) & 0x1fU;
	uint32_t gpio4_ext = (sys_read32(0xfffff52050UL) >> 8) & 0x1fU;

	printk("  Pattern 0b10101 -> gpio4 DR[12:8]=0x%02x, EXT[12:8]=0x%02x\n",
	       gpio4_dr, gpio4_ext);
	if (gpio4_dr != 0x15U || gpio4_ext != 0x15U) {
		printk("  [FAIL] gpio4 register mismatch for pattern 0x15\n");
		return -1;
	}

	/* Turn all 5 user LEDs ON (0x1f) */
	for (size_t i = 0; i < ARRAY_SIZE(usr_leds); i++) {
		gpio_pin_set_dt(&usr_leds[i], 1);
	}
	gpio4_dr = (sys_read32(0xfffff52000UL) >> 8) & 0x1fU;
	if (gpio4_dr != 0x1fU) {
		printk("  [FAIL] gpio4 register mismatch for pattern 0x1f\n");
		return -1;
	}

	/* Also toggle AP6203BM WL_REG_ON (gpio2 pin 31) and BT_REG_ON (gpio2 pin 28) */
	const struct device *gpio2 = DEVICE_DT_GET(DT_NODELABEL(gpio2));

	if (!device_is_ready(gpio2)) {
		printk("  [FAIL] gpio2 not ready\n");
		return -1;
	}
	gpio_pin_configure(gpio2, 31, GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure(gpio2, 28, GPIO_OUTPUT_ACTIVE);
	uint32_t gpio2_dr = sys_read32(0xffe7f34000UL);

	if ((gpio2_dr & (BIT(31) | BIT(28))) != (BIT(31) | BIT(28))) {
		printk("  [FAIL] AP6203BM GPIO2 control lines did not assert\n");
		return -1;
	}

	printk("  [PASS] 5x User LEDs (USR0..USR4) on gpio4 & AP6203BM lines on gpio2 verified\n");
	return 0;
}

/* 4. DesignWare I2C0 & Onboard 4 KiB AT24C32 EEPROM at 0x50 */
static int verify_i2c_eeprom(void)
{
	printk("\n[4/8] DesignWare I2C0 & Onboard 4 KiB AT24C32 EEPROM (0x50) Verification\n");
	const struct device *eeprom = DEVICE_DT_GET(DT_ALIAS(eeprom_0));

	if (!device_is_ready(eeprom)) {
		printk("  [FAIL] eeprom0 device not ready\n");
		return -1;
	}

	size_t size = eeprom_get_size(eeprom);

	printk("  EEPROM size: %zu bytes\n", size);
	if (size != 4096) {
		printk("  [FAIL] Expected 4096-byte EEPROM\n");
		return -1;
	}

	const char tx_str[] = "BeagleV-Ahead Zephyr RTOS TH1520 EEPROM OK!";
	char rx_str[sizeof(tx_str)] = {0};

	/* Write across 32-byte page boundary starting at offset 0x0018 */
	int ret = eeprom_write(eeprom, 0x0018, tx_str, sizeof(tx_str));

	if (ret != 0) {
		printk("  [FAIL] eeprom_write failed: %d\n", ret);
		return -1;
	}

	ret = eeprom_read(eeprom, 0x0018, rx_str, sizeof(rx_str));
	if (ret != 0) {
		printk("  [FAIL] eeprom_read failed: %d\n", ret);
		return -1;
	}

	printk("  EEPROM readback @ 0x0018: \"%s\"\n", rx_str);
	if (memcmp(tx_str, rx_str, sizeof(tx_str)) != 0) {
		printk("  [FAIL] EEPROM data mismatch\n");
		return -1;
	}

	printk("  [PASS] Onboard I2C0 AT24C32 EEPROM cross-page write/read verified\n");
	return 0;
}

/* 5. AON I2C & Onboard Dialog DA9063 PMIC at 0x5a */
#define DA9063_I2C_ADDR       0x5a
#define DA9063_REG_DVC_1      0x32
#define DA9063_REG_DVC_2      0x33
#define DA9063_REG_VBCORE2_A  0xa3
#define DA9063_REG_VBCORE1_A  0xa4

static int verify_aon_i2c_pmic(void)
{
	printk("\n[5/8] AON I2C & Onboard Dialog DA9063 PMIC (0x5a) Verification\n");
	const struct device *aon_i2c = DEVICE_DT_GET(DT_NODELABEL(aon_i2c));

	if (!device_is_ready(aon_i2c)) {
		printk("  [FAIL] aon_i2c not ready\n");
		return -1;
	}

	int ret = i2c_reg_write_byte(aon_i2c, DA9063_I2C_ADDR, DA9063_REG_VBCORE1_A, 0x7a);

	ret |= i2c_reg_write_byte(aon_i2c, DA9063_I2C_ADDR, DA9063_REG_VBCORE2_A, 0x78);
	ret |= i2c_reg_write_byte(aon_i2c, DA9063_I2C_ADDR, DA9063_REG_DVC_2, 0xff);
	if (ret != 0) {
		printk("  [FAIL] DA9063 I2C register write failed: %d\n", ret);
		return -1;
	}

	uint8_t vbcore1 = 0, vbcore2 = 0, dvc2 = 0;

	ret = i2c_reg_read_byte(aon_i2c, DA9063_I2C_ADDR, DA9063_REG_VBCORE1_A, &vbcore1);
	ret |= i2c_reg_read_byte(aon_i2c, DA9063_I2C_ADDR, DA9063_REG_VBCORE2_A, &vbcore2);
	ret |= i2c_reg_read_byte(aon_i2c, DA9063_I2C_ADDR, DA9063_REG_DVC_2, &dvc2);
	if (ret != 0) {
		printk("  [FAIL] DA9063 I2C register read failed: %d\n", ret);
		return -1;
	}

	printk("  DA9063 VBCORE1_A=0x%02x, VBCORE2_A=0x%02x, DVC_2=0x%02x (masked 0x81)\n",
	       vbcore1, vbcore2, dvc2);
	if (vbcore1 != 0x7a || vbcore2 != 0x78 || dvc2 != 0x81) {
		printk("  [FAIL] Unexpected DA9063 register values\n");
		return -1;
	}

	printk("  [PASS] AON I2C & Dialog DA9063 PMIC verified\n");
	return 0;
}

/* 6. DesignWare SPI0 (mikrobus_spi) */
static int verify_spi(void)
{
	printk("\n[6/8] DesignWare SPI0 (mikrobus_spi) Verification\n");
	const struct device *spi = DEVICE_DT_GET(DT_NODELABEL(mikrobus_spi));

	if (!device_is_ready(spi)) {
		printk("  [FAIL] mikrobus_spi (spi0) not ready\n");
		return -1;
	}

	struct spi_config cfg = {
		.frequency = 10000000U,
		.operation = SPI_WORD_SET(8) | SPI_LINES_SINGLE,
		.slave = 0,
	};
	uint8_t tx_buf[8] = {0x9f, 0x01, 0x02, 0x03, 0xa5, 0x5a, 0xf0, 0x0f};
	uint8_t rx_buf[8] = {0};
	const struct spi_buf tx_b = {.buf = tx_buf, .len = sizeof(tx_buf)};
	const struct spi_buf rx_b = {.buf = rx_buf, .len = sizeof(rx_buf)};
	const struct spi_buf_set tx_set = {.buffers = &tx_b, .count = 1};
	const struct spi_buf_set rx_set = {.buffers = &rx_b, .count = 1};

	int ret = spi_transceive(spi, &cfg, &tx_set, &rx_set);

	if (ret != 0) {
		printk("  [FAIL] spi_transceive failed: %d\n", ret);
		return -1;
	}

	printk("  Transceived %zu bytes on spi0 @ 10 MHz via PLIC IRQ 54\n", sizeof(tx_buf));
	printk("  [PASS] DesignWare SPI0 (mikrobus_spi) verified\n");
	return 0;
}

/* 7. DesignWare APB Timer (timer0, PLIC IRQ 16) */
static K_SEM_DEFINE(timer0_sem, 0, 1);
static volatile uint32_t timer0_irq_ticks;

static void timer0_alarm_cb(const struct device *dev, uint8_t chan_id,
			    uint32_t ticks, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan_id);
	ARG_UNUSED(user_data);

	timer0_irq_ticks = ticks;
	k_sem_give(&timer0_sem);
}

static int verify_dw_timer(void)
{
	printk("\n[7/8] DesignWare APB Timer0 (PLIC IRQ 16 @ 125 MHz) Verification\n");
	const struct device *timer0 = DEVICE_DT_GET(DT_NODELABEL(timer0));

	if (!device_is_ready(timer0)) {
		printk("  [FAIL] timer0 not ready\n");
		return -1;
	}

	int ret = counter_start(timer0);

	if (ret != 0) {
		printk("  [FAIL] counter_start failed: %d\n", ret);
		return -1;
	}

	struct counter_alarm_cfg alarm_cfg = {
		.flags = 0,
		.ticks = counter_us_to_ticks(timer0, 5000), /* 5 ms = 625,000 ticks */
		.callback = timer0_alarm_cb,
		.user_data = NULL,
	};

	ret = counter_set_channel_alarm(timer0, 0, &alarm_cfg);
	if (ret != 0) {
		printk("  [FAIL] counter_set_channel_alarm failed: %d\n", ret);
		return -1;
	}

	if (k_sem_take(&timer0_sem, K_MSEC(500)) != 0) {
		printk("  [FAIL] Timeout waiting for timer0 alarm interrupt (PLIC IRQ 16)\n");
		return -1;
	}

	counter_stop(timer0);
	printk("  Timer0 alarm fired via PLIC IRQ 16 (configured %u ticks)\n", alarm_cfg.ticks);
	printk("  [PASS] DesignWare APB Timer0 verified\n");
	return 0;
}

/* 8. DesignWare Watchdog (wdt0, PLIC IRQ 24) */
static K_SEM_DEFINE(wdt0_sem, 0, 1);

static void wdt0_isr_cb(const struct device *dev, int channel_id)
{
	ARG_UNUSED(channel_id);
	wdt_feed(dev, 0);
	/* Disarm wdt0 via TH1520 AP_RESET WDT0 register (0xffef014034 bit 0 active-low) */
	sys_write32(0U, 0xffef014034UL);
	sys_write32(1U, 0xffef014034UL);
	k_sem_give(&wdt0_sem);
}

static int verify_watchdog(void)
{
	printk("\n[8/8] DesignWare Watchdog0 (wdt0 @ 125 MHz, PLIC IRQ 24) Verification\n");
	const struct device *wdt0 = DEVICE_DT_GET(DT_ALIAS(watchdog0));

	if (!device_is_ready(wdt0)) {
		printk("  [FAIL] wdt0 not ready\n");
		return -1;
	}

	struct wdt_timeout_cfg wdt_cfg = {
		.window = {.min = 0U, .max = 5U},
		.callback = wdt0_isr_cb,
		.flags = WDT_FLAG_RESET_SOC,
	};

	int chan_id = wdt_install_timeout(wdt0, &wdt_cfg);

	if (chan_id < 0) {
		printk("  [FAIL] wdt_install_timeout failed: %d\n", chan_id);
		return -1;
	}

	int ret = wdt_setup(wdt0, 0);

	if (ret != 0) {
		printk("  [FAIL] wdt_setup failed: %d\n", ret);
		return -1;
	}

	for (int i = 0; i < 3; i++) {
		ret = wdt_feed(wdt0, chan_id);
		if (ret != 0) {
			printk("  [FAIL] wdt_feed failed: %d\n", ret);
			return -1;
		}
	}

	if (k_sem_take(&wdt0_sem, K_MSEC(500)) != 0) {
		printk("  [FAIL] Timeout waiting for wdt0 interrupt (PLIC IRQ 24)\n");
		return -1;
	}

	printk("  Watchdog0 armed, fed 3x, fired PLIC IRQ 24 & disarmed via AP_RESET\n");
	printk("  [PASS] DesignWare Watchdog0 verified\n");
	return 0;
}

int main(void)
{
	printk("================================================================\n");
	printk(" BeagleV-Ahead (T-Head TH1520) Zephyr RTOS Hardware Verification\n");
	printk(" Board Target: %s\n", CONFIG_BOARD_TARGET);
	printk("================================================================\n");

	int failures = 0;

	failures += (verify_cpu_and_smp() != 0);
	failures += (verify_uart_irq() != 0);
	failures += (verify_gpio_and_leds() != 0);
	failures += (verify_i2c_eeprom() != 0);
	failures += (verify_aon_i2c_pmic() != 0);
	failures += (verify_spi() != 0);
	failures += (verify_dw_timer() != 0);
	failures += (verify_watchdog() != 0);

	printk("\n================================================================\n");
	if (failures == 0) {
		printk(" ALL 8/8 BEAGLEV-AHEAD HARDWARE SUBSYSTEMS PASSED!\n");
	} else {
		printk(" VERIFICATION FAILED (%d/8 subsystems failed)\n", failures);
	}
	printk("================================================================\n");

	return 0;
}
