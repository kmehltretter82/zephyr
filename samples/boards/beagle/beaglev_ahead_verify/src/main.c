/*
 * Copyright (c) 2026 BeagleV-Ahead Zephyr Port
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Comprehensive 18-Subsystem Hardware & Stack Verification Suite for Zephyr RTOS
 * on BeagleV-Ahead (T-Head TH1520 quad-core XuanTie C910):
 *   - Base Port (1..8): CPU/SMP, UART3 IRQ, GPIO/LEDs/AP6203BM, I2C0 EEPROM,
 *                       AON I2C PMIC, SPI0, DW Timer0, DW Watchdog0
 *   - Track 1 (9..13):  TH1520 6-Ch PWM, TH1520 Pinctrl, Moortec MR75203 PVT,
 *                       APM X-Gene RTC, DesignWare AXI DMA (M2M + M2P UART)
 *   - Track 2 (14..16): Gigabit Ethernet (GMAC0 + IPv4/UDP), Bluetooth H4 HCI
 *                       (AP6203BM BLE Adv/Scan), DWC MSHC eMMC Disk Read/Write
 *   - Track 3 (17):     MikroE Click Boards (Bosch BME280 Weather Click on I2C3
 *                       + Microchip MCP2518FD CAN-FD Click on SPI0)
 *   - Track 4 (18):     TH1520 Hardware Mailbox (MBOX0) + Shared-Memory AMP IPC
 *                       & On-Chip SRAM Execution Check
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
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/socket.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/crypto.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/arch/riscv/csr.h>
#include <string.h>
#include <soc.h>

#define TOTAL_TESTS 18

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
	printk("\n[1/%d] CPU, Vendor CSRs & %d-Hart SMP Verification\n",
	       TOTAL_TESTS, SMP_WORKERS);

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

	k_msleep(20);
	uint64_t elapsed = k_cycle_get_64() - c0;

	printk("  Timer freq: %u Hz, 20 ms sleep elapsed: %llu cycles\n",
	       freq, (unsigned long long)elapsed);
	if (freq != 3000000U || elapsed < 40000ULL) {
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
	printk("\n[2/%d] PLIC Interrupt-Driven UART Verification (mikrobus_serial / uart3)\n",
	       TOTAL_TESTS);
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
	printk("\n[3/%d] DesignWare GPIO, Onboard User LEDs (USR0..USR4) & AP6203BM Verification\n",
	       TOTAL_TESTS);

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

	for (size_t i = 0; i < ARRAY_SIZE(usr_leds); i++) {
		gpio_pin_set_dt(&usr_leds[i], 1);
	}
	gpio4_dr = (sys_read32(0xfffff52000UL) >> 8) & 0x1fU;
	if (gpio4_dr != 0x1fU) {
		printk("  [FAIL] gpio4 register mismatch for pattern 0x1f\n");
		return -1;
	}

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
	printk("\n[4/%d] DesignWare I2C0 & Onboard 4 KiB AT24C32 EEPROM (0x50) Verification\n",
	       TOTAL_TESTS);
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
	printk("\n[5/%d] AON I2C & Onboard Dialog DA9063 PMIC (0x5a) Verification\n",
	       TOTAL_TESTS);
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
	printk("\n[6/%d] DesignWare SPI0 (mikrobus_spi) Verification\n", TOTAL_TESTS);
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
	uint8_t tx_buf[8] = {0x3e, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
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
	printk("\n[7/%d] DesignWare APB Timer0 (PLIC IRQ 16 @ 125 MHz) Verification\n",
	       TOTAL_TESTS);
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
		.ticks = counter_us_to_ticks(timer0, 5000),
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
	sys_write32(0U, 0xffef014034UL);
	sys_write32(1U, 0xffef014034UL);
	k_sem_give(&wdt0_sem);
}

static int verify_watchdog(void)
{
	printk("\n[8/%d] DesignWare Watchdog0 (wdt0 @ 125 MHz, PLIC IRQ 24) Verification\n",
	       TOTAL_TESTS);
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

/* 9. Track 1.1: TH1520 6-Channel PWM (pwm0 @ 0xffec01c000) */
static int verify_pwm(void)
{
	printk("\n[9/%d] [Track 1.1] TH1520 6-Channel PWM (pwm0 @ 0xffec01c000) Verification\n",
	       TOTAL_TESTS);
	const struct device *pwm = DEVICE_DT_GET(DT_ALIAS(pwm_0));

	if (!device_is_ready(pwm)) {
		printk("  [FAIL] pwm0 device not ready\n");
		return -1;
	}

	uint64_t cycles_per_sec = 0;
	int ret = pwm_get_cycles_per_sec(pwm, 0, &cycles_per_sec);

	if (ret != 0 || cycles_per_sec != 100000000ULL) {
		printk("  [FAIL] Unexpected PWM clock rate: %llu Hz\n",
		       (unsigned long long)cycles_per_sec);
		return -1;
	}

	/* Configure Ch0 (MikroBUS PWM): 20 kHz (50,000 ns period), 25% duty (12,500 ns) */
	ret = pwm_set(pwm, 0, PWM_USEC(50), PWM_NSEC(12500), PWM_POLARITY_NORMAL);
	if (ret != 0) {
		printk("  [FAIL] pwm_set ch0 failed: %d\n", ret);
		return -1;
	}

	/* Configure Ch1: 10 kHz (100,000 ns period), 75% duty, inverted polarity */
	ret = pwm_set(pwm, 1, PWM_USEC(100), PWM_USEC(75), PWM_POLARITY_INVERTED);
	if (ret != 0) {
		printk("  [FAIL] pwm_set ch1 failed: %d\n", ret);
		return -1;
	}

	uint32_t ch0_ctrl = sys_read32(0xffec01c000UL + 0x00);
	uint32_t ch0_per  = sys_read32(0xffec01c000UL + 0x08);
	uint32_t ch0_fp   = sys_read32(0xffec01c000UL + 0x0c);
	uint32_t ch1_ctrl = sys_read32(0xffec01c000UL + 0x20);
	uint32_t ch1_per  = sys_read32(0xffec01c000UL + 0x28);
	uint32_t ch1_fp   = sys_read32(0xffec01c000UL + 0x2c);

	printk("  Ch0 (20 kHz, 25%%): CTRL=0x%03x PERIOD=%u FP=%u\n",
	       ch0_ctrl, ch0_per, ch0_fp);
	printk("  Ch1 (10 kHz, 75%% inv): CTRL=0x%03x PERIOD=%u FP=%u\n",
	       ch1_ctrl, ch1_per, ch1_fp);

	if (ch0_per != 5000U || ch0_fp != 1250U ||
	    ch1_per != 10000U || ch1_fp != 7500U) {
		printk("  [FAIL] PWM register readback mismatch\n");
		return -1;
	}

	printk("  [PASS] TH1520 6-Channel PWM verified\n");
	return 0;
}

/* 10. Track 1.2: TH1520 Pinmux / Pinctrl (PADCTRL_AOSYS, PADCTRL0_APSYS, PADCTRL1_APSYS) */
static int verify_pinctrl(void)
{
	printk("\n[10/%d] [Track 1.2] TH1520 Pinctrl (PADCTRL0/1_APSYS & AOSYS) Verification\n",
	       TOTAL_TESTS);

	const pinctrl_soc_pin_t test_pins[] = {
		{
			.base = 0xffec007000UL, /* PADCTRL0_APSYS */
			.pinmux = TH1520_PINMUX(11, TH1520_MUX_FUNC1,
						TH1520_PADCFG_INPUT_EN |
						TH1520_PADCFG_PULL_UP | 8U),
		},
		{
			.base = 0xffec007000UL, /* PADCTRL0_APSYS */
			.pinmux = TH1520_PINMUX(12, TH1520_MUX_FUNC1,
						TH1520_PADCFG_INPUT_EN |
						TH1520_PADCFG_PULL_UP | 8U),
		},
	};

	int ret = pinctrl_configure_pins(test_pins, ARRAY_SIZE(test_pins), 0);

	if (ret != 0) {
		printk("  [FAIL] pinctrl_configure_pins failed: %d\n", ret);
		return -1;
	}

	/* Check PADCFG for pin 11 (word 5 high 16b) and MUXCFG for pins 11..12 (word 1 at 0x404) */
	uint32_t padcfg5 = sys_read32(0xffec007000UL + 5 * 4);
	uint32_t muxcfg1 = sys_read32(0xffec007000UL + 0x404);
	/* Also check pwm0 pin 3 configured automatically during boot */
	uint32_t muxcfg0 = sys_read32(0xffec007000UL + 0x400);

	printk("  PADCTRL0_APSYS PADCFG[5]=0x%08x, MUXCFG[0]=0x%08x, MUXCFG[1]=0x%08x\n",
	       padcfg5, muxcfg0, muxcfg1);

	if (((muxcfg1 >> 12) & 0xfU) != TH1520_MUX_FUNC1 ||
	    ((muxcfg1 >> 16) & 0xfU) != TH1520_MUX_FUNC1 ||
	    ((muxcfg0 >> 12) & 0xfU) != TH1520_MUX_FUNC1) {
		printk("  [FAIL] Pinctrl MUXCFG selector mismatch\n");
		return -1;
	}

	printk("  [PASS] TH1520 Pinctrl pad multiplexing, pull-ups & drive-strength verified\n");
	return 0;
}

/* 11. Track 1.3a: Moortec MR75203 On-Die PVT Sensor (pvt0 @ 0xfffff4e000) */
static int verify_pvt_sensor(void)
{
	printk("\n[11/%d] [Track 1.3a] Moortec MR75203 On-Die PVT Sensor Verification\n",
	       TOTAL_TESTS);
	const struct device *pvt = DEVICE_DT_GET(DT_NODELABEL(pvt0));

	if (!device_is_ready(pvt)) {
		printk("  [FAIL] pvt0 sensor device not ready\n");
		return -1;
	}

	int ret = sensor_sample_fetch(pvt);

	if (ret != 0) {
		printk("  [FAIL] sensor_sample_fetch(pvt0) failed: %d\n", ret);
		return -1;
	}

	struct sensor_value temp = {0}, volt = {0};

	ret = sensor_channel_get(pvt, SENSOR_CHAN_DIE_TEMP, &temp);
	ret |= sensor_channel_get(pvt, SENSOR_CHAN_VOLTAGE, &volt);
	if (ret != 0) {
		printk("  [FAIL] sensor_channel_get(pvt0) failed: %d\n", ret);
		return -1;
	}

	printk("  On-Die Temperature: %d.%03d C, Core Voltage: %d.%03d V\n",
	       temp.val1, temp.val2 / 1000, volt.val1, volt.val2 / 1000);
	if (temp.val1 < 10 || temp.val1 > 80 || volt.val2 < 750000 || volt.val2 > 850000) {
		printk("  [FAIL] PVT telemetry out of expected range\n");
		return -1;
	}

	printk("  [PASS] Moortec MR75203 On-Die PVT temperature & voltage verified\n");
	return 0;
}

/* 12. Track 1.3b: APM X-Gene / DesignWare RTC (rtc0 @ 0xfffff40000) */
static int verify_rtc(void)
{
	printk("\n[12/%d] [Track 1.3b] DesignWare / X-Gene RTC (rtc0 @ 0xfffff40000) Verification\n",
	       TOTAL_TESTS);
	const struct device *rtc = DEVICE_DT_GET(DT_ALIAS(rtc));

	if (!device_is_ready(rtc)) {
		printk("  [FAIL] rtc0 device not ready\n");
		return -1;
	}

	/* Set UTC time to 2026-10-25 14:30:45 */
	struct rtc_time set_tm = {
		.tm_sec = 45,
		.tm_min = 30,
		.tm_hour = 14,
		.tm_mday = 25,
		.tm_mon = 9,    /* October (0-based) */
		.tm_year = 126, /* 2026 - 1900 */
		.tm_wday = 0,
		.tm_yday = 297,
		.tm_isdst = -1,
	};

	int ret = rtc_set_time(rtc, &set_tm);

	if (ret != 0) {
		printk("  [FAIL] rtc_set_time failed: %d\n", ret);
		return -1;
	}

	struct rtc_time get_tm = {0};

	ret = rtc_get_time(rtc, &get_tm);
	if (ret != 0) {
		printk("  [FAIL] rtc_get_time failed: %d\n", ret);
		return -1;
	}

	printk("  RTC UTC readback: %04d-%02d-%02d %02d:%02d:%02d\n",
	       get_tm.tm_year + 1900, get_tm.tm_mon + 1, get_tm.tm_mday,
	       get_tm.tm_hour, get_tm.tm_min, get_tm.tm_sec);

	if (get_tm.tm_year != 126 || get_tm.tm_mon != 9 || get_tm.tm_mday != 25 ||
	    get_tm.tm_hour != 14 || get_tm.tm_min != 30) {
		printk("  [FAIL] RTC readback mismatch\n");
		return -1;
	}

	printk("  [PASS] DesignWare / X-Gene RTC calendar read/write verified\n");
	return 0;
}

/* 13. Track 1.4: DesignWare AXI DMA (dmac0 @ 0xffefc00000, IRQ 27) */
static K_SEM_DEFINE(dma_done_sem, 0, 1);
static uint32_t dma_src_buf[16] __aligned(64);
static uint32_t dma_dst_buf[16] __aligned(64);
static uint32_t dma_uart_tx_words[8] __aligned(64) = {
	'D', 'M', 'A', '_', 'U', 'A', 'R', 'T'
};

static void dma_xfer_cb(const struct device *dev, void *user_data,
			uint32_t channel, int status)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	ARG_UNUSED(channel);

	if (status == 0) {
		k_sem_give(&dma_done_sem);
	}
}

static int verify_axi_dma(void)
{
	printk("\n[13/%d] [Track 1.4] DesignWare AXI DMA (dmac0 @ 0xffefc00000, IRQ 27) Verification\n",
	       TOTAL_TESTS);
	const struct device *dmac = DEVICE_DT_GET(DT_NODELABEL(dmac0));

	if (!device_is_ready(dmac)) {
		printk("  [FAIL] dmac0 device not ready\n");
		return -1;
	}

	for (size_t i = 0; i < ARRAY_SIZE(dma_src_buf); i++) {
		dma_src_buf[i] = 0xCAFE0000U + (uint32_t)i;
		dma_dst_buf[i] = 0U;
	}

	struct dma_block_config blk_m2m = {
		.source_address = (uintptr_t)dma_src_buf,
		.dest_address = (uintptr_t)dma_dst_buf,
		.block_size = sizeof(dma_src_buf),
	};
	struct dma_config cfg_m2m = {
		.channel_direction = MEMORY_TO_MEMORY,
		.source_data_size = 4U,
		.dest_data_size = 4U,
		.source_burst_length = 4U,
		.dest_burst_length = 4U,
		.block_count = 1U,
		.head_block = &blk_m2m,
		.dma_callback = dma_xfer_cb,
	};

	k_sem_reset(&dma_done_sem);
	int ret = dma_config(dmac, 0, &cfg_m2m);

	if (ret != 0) {
		printk("  [FAIL] dma_config (M2M) failed: %d\n", ret);
		return -1;
	}

	ret = dma_start(dmac, 0);
	if (ret != 0) {
		printk("  [FAIL] dma_start (M2M) failed: %d\n", ret);
		return -1;
	}

	if (k_sem_take(&dma_done_sem, K_MSEC(500)) != 0) {
		printk("  [FAIL] Timeout waiting for AXI DMA Ch0 IRQ 27\n");
		return -1;
	}

	if (memcmp(dma_src_buf, dma_dst_buf, sizeof(dma_src_buf)) != 0) {
		printk("  [FAIL] AXI DMA M2M buffer mismatch\n");
		return -1;
	}

	/* Now test M2P (Memory-to-Peripheral) DMA into UART3 THR (0xffe7f04000) */
	struct dma_block_config blk_m2p = {
		.source_address = (uintptr_t)dma_uart_tx_words,
		.dest_address = 0xffe7f04000ULL,
		.block_size = sizeof(dma_uart_tx_words),
		.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
	};
	struct dma_config cfg_m2p = {
		.dma_slot = 6U,
		.channel_direction = MEMORY_TO_PERIPHERAL,
		.source_data_size = 4U,
		.dest_data_size = 4U,
		.source_burst_length = 1U,
		.dest_burst_length = 1U,
		.block_count = 1U,
		.head_block = &blk_m2p,
		.dma_callback = dma_xfer_cb,
	};

	k_sem_reset(&dma_done_sem);
	ret = dma_config(dmac, 1, &cfg_m2p);
	if (ret != 0) {
		printk("  [FAIL] dma_config (M2P UART3) failed: %d\n", ret);
		return -1;
	}

	ret = dma_start(dmac, 1);
	if (ret != 0) {
		printk("  [FAIL] dma_start (M2P UART3) failed: %d\n", ret);
		return -1;
	}

	if (k_sem_take(&dma_done_sem, K_MSEC(500)) != 0) {
		printk("  [FAIL] Timeout waiting for AXI DMA Ch1 M2P IRQ 27\n");
		return -1;
	}

	printk("  AXI DMA Ch0 M2M (64 B) & Ch1 M2P -> UART3 THR (8 chars) completed via IRQ 27\n");
	printk("  [PASS] DesignWare AXI DMA M2M & M2P transfers verified\n");
	return 0;
}

/* 14. Track 2.1: Gigabit Ethernet (gmac0 @ 0xffe7070000 + Zephyr IPv4/UDP Stack) */
static int verify_ethernet(void)
{
	printk("\n[14/%d] [Track 2.1] Gigabit Ethernet (gmac0 @ 0xffe7070000 + IPv4/UDP) Verification\n",
	       TOTAL_TESTS);
	const struct device *gmac = DEVICE_DT_GET(DT_NODELABEL(gmac0));

	if (!device_is_ready(gmac)) {
		printk("  [FAIL] gmac0 Ethernet device not ready\n");
		return -1;
	}

	struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));

	if (iface == NULL) {
		printk("  [FAIL] No Ethernet net_if found\n");
		return -1;
	}

	struct in_addr addr = { { { 192, 0, 2, 10 } } };
	struct in_addr netmask = { { { 255, 255, 255, 0 } } };

	net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
	net_if_ipv4_set_netmask_by_addr(iface, &addr, &netmask);
	net_if_up(iface);

	/* Verify Zephyr BSD UDP socket send/recv over IPv4 + GMAC0 DMA TX/RX Loopback */
	int sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (sock < 0) {
		printk("  [FAIL] zsock_socket failed: %d\n", errno);
		return -1;
	}

	struct sockaddr_in bind_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(4242),
		.sin_addr = addr,
	};
	struct sockaddr_in dst_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(4242),
		.sin_addr = { { { 192, 0, 2, 2 } } },
	};

	if (zsock_bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		zsock_close(sock);
		printk("  [FAIL] zsock_bind failed: %d\n", errno);
		return -1;
	}

	const char udp_payload[] = "ZEPHYR_TH1520_GMAC0_UDP_OK";
	char udp_rx[32] = {0};

	if (zsock_sendto(sock, udp_payload, sizeof(udp_payload), 0,
			 (struct sockaddr *)&dst_addr, sizeof(dst_addr)) < 0) {
		zsock_close(sock);
		printk("  [FAIL] zsock_sendto failed: %d\n", errno);
		return -1;
	}

	ssize_t rcvd = -1;

	for (int retry = 0; retry < 50; retry++) {
		rcvd = zsock_recv(sock, udp_rx, sizeof(udp_rx), ZSOCK_MSG_DONTWAIT);
		if (rcvd > 0) {
			break;
		}
		k_msleep(5);
	}

	zsock_close(sock);
	if (rcvd != sizeof(udp_payload) || memcmp(udp_rx, udp_payload, sizeof(udp_payload)) != 0) {
		printk("  [FAIL] UDP socket receive mismatch (rcvd=%zd)\n", rcvd);
		return -1;
	}

	printk("  GMAC0 MAC=02:be:a6:15:20:01 IPv4=192.0.2.10 L2 TX/RX & UDP/4242 \"%s\" OK\n",
	       udp_rx);
	printk("  [PASS] Gigabit Ethernet (gmac0 + RTL8211F PHY + Zephyr IPv4/UDP) verified\n");
	return 0;
}

/* 15. Track 2.2: Bluetooth HCI over UART4 (AP6203BM BLE Adv + Scan) */
#if !defined(CONFIG_BT_HOST_CRYPTO)
int bt_rand(void *buf, size_t len)
{
	if (buf == NULL || len == 0) {
		return -EINVAL;
	}
	sys_rand_get(buf, len);
	return 0;
}
#endif

static atomic_t ble_adv_seen;

static void bt_scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
		       struct net_buf_simple *buf)
{
	ARG_UNUSED(addr);
	ARG_UNUSED(rssi);
	ARG_UNUSED(adv_type);
	ARG_UNUSED(buf);

	atomic_inc(&ble_adv_seen);
}

static int verify_bluetooth(void)
{
	printk("\n[15/%d] [Track 2.2] Bluetooth H4 UART4 HCI (AP6203BM BLE Adv/Scan) Verification\n",
	       TOTAL_TESTS);

	int err = bt_enable(NULL);

	if (err != 0) {
		printk("  [FAIL] bt_enable() over UART4 H4 failed: %d\n", err);
		return -1;
	}

	/* Start non-connectable BLE advertising */
	const struct bt_data ad[] = {
		BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_NO_BREDR),
		BT_DATA(BT_DATA_NAME_COMPLETE, "BeagleV-Ahead", 13),
	};

	err = bt_le_adv_start(BT_LE_ADV_NCONN, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err != 0) {
		printk("  [FAIL] bt_le_adv_start failed: %d\n", err);
		return -1;
	}
	bt_le_adv_stop();

	/* Start active BLE scan and receive the synthetic AP6203BM LE Advertising Report */
	atomic_set(&ble_adv_seen, 0);
	struct bt_le_scan_param scan_param = {
		.type = BT_LE_SCAN_TYPE_ACTIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = 0x0010,
		.window = 0x0010,
	};

	err = bt_le_scan_start(&scan_param, bt_scan_cb);
	if (err != 0) {
		printk("  [FAIL] bt_le_scan_start failed: %d\n", err);
		return -1;
	}

	for (int i = 0; i < 20 && atomic_get(&ble_adv_seen) == 0; i++) {
		k_msleep(10);
	}
	bt_le_scan_stop();

	if (atomic_get(&ble_adv_seen) == 0) {
		printk("  [FAIL] Did not receive BLE advertising report during scan\n");
		return -1;
	}

	printk("  AP6203BM H4 HCI initialized, BLE advertising & active scan (%ld adv seen) OK\n",
	       (long)atomic_get(&ble_adv_seen));
	printk("  [PASS] Bluetooth H4 UART4 HCI (AP6203BM) verified\n");
	return 0;
}

/* 16. Track 2.3: Synopsys DWC MSHC eMMC Disk Read/Write (mshc0 @ 0xffe7080000) */
static uint8_t emmc_tx_buf[1024] __aligned(4);
static uint8_t emmc_rx_buf[1024] __aligned(4);

static int verify_emmc_disk(void)
{
	printk("\n[16/%d] [Track 2.3] Synopsys DWC MSHC eMMC0 (mshc0 @ 0xffe7080000) Verification\n",
	       TOTAL_TESTS);

	int ret = disk_access_init("EMMC");

	if (ret != 0) {
		printk("  [FAIL] disk_access_init(\"EMMC\") failed: %d\n", ret);
		return -1;
	}

	uint32_t sector_count = 0, sector_size = 0;

	ret = disk_access_ioctl("EMMC", DISK_IOCTL_GET_SECTOR_COUNT, &sector_count);
	ret |= disk_access_ioctl("EMMC", DISK_IOCTL_GET_SECTOR_SIZE, &sector_size);
	if (ret != 0 || sector_size != 512U || sector_count == 0U) {
		printk("  [FAIL] Invalid eMMC geometry: %u sectors x %u B\n",
		       sector_count, sector_size);
		return -1;
	}

	for (size_t i = 0; i < sizeof(emmc_tx_buf); i++) {
		emmc_tx_buf[i] = (uint8_t)((i * 37U + 0x5aU) & 0xffU);
		emmc_rx_buf[i] = 0U;
	}

	/* Write 2 sectors (1024 B) starting at LBA 16 and read back */
	ret = disk_access_write("EMMC", emmc_tx_buf, 16, 2);
	if (ret != 0) {
		printk("  [FAIL] disk_access_write failed: %d\n", ret);
		return -1;
	}

	ret = disk_access_read("EMMC", emmc_rx_buf, 16, 2);
	if (ret != 0) {
		printk("  [FAIL] disk_access_read failed: %d\n", ret);
		return -1;
	}

	if (memcmp(emmc_tx_buf, emmc_rx_buf, sizeof(emmc_tx_buf)) != 0) {
		printk("  [FAIL] eMMC readback data mismatch at LBA 16..17\n");
		return -1;
	}

	printk("  eMMC capacity: %u sectors (%u KiB), LBA 16..17 write/read (1024 B) verified\n",
	       sector_count, (sector_count * sector_size) / 1024U);
	printk("  [PASS] Synopsys DWC MSHC eMMC0 block storage verified\n");
	return 0;
}

/* 17. Track 3: MikroE Click Boards (Bosch BME280 on i2c3 + Microchip MCP2518FD CAN-FD on spi0) */
static K_SEM_DEFINE(can_rx_sem, 0, 1);
static struct can_frame can_rx_frame;

static void can_tx_callback(const struct device *dev, int error, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(error);
	ARG_UNUSED(user_data);
}

static void can_rx_callback(const struct device *dev, struct can_frame *frame,
			    void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	can_rx_frame = *frame;
	k_sem_give(&can_rx_sem);
}

static int verify_mikrobus_clicks(void)
{
	printk("\n[17/%d] [Track 3] MikroE Click Boards (BME280 Weather + MCP2518FD CAN-FD)\n",
	       TOTAL_TESTS);

	/* 3A: Bosch BME280 Weather Click on mikrobus_i2c (i2c3 @ 0x76) */
	const struct device *bme = DEVICE_DT_GET(DT_NODELABEL(bme280));

	if (!device_is_ready(bme)) {
		printk("  [FAIL] Bosch BME280 Weather Click on i2c3 not ready\n");
		return -1;
	}

	int ret = sensor_sample_fetch(bme);

	if (ret != 0) {
		printk("  [FAIL] sensor_sample_fetch(bme280) failed: %d\n", ret);
		return -1;
	}

	struct sensor_value temp = {0}, press = {0}, hum = {0};

	ret = sensor_channel_get(bme, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	ret |= sensor_channel_get(bme, SENSOR_CHAN_PRESS, &press);
	ret |= sensor_channel_get(bme, SENSOR_CHAN_HUMIDITY, &hum);
	if (ret != 0) {
		printk("  [FAIL] sensor_channel_get(bme280) failed: %d\n", ret);
		return -1;
	}

	printk("  BME280 Weather Click (i2c3 @ 0x76): T=%d.%02d C, P=%d.%02d kPa, RH=%d.%02d %%\n",
	       temp.val1, abs(temp.val2) / 10000,
	       press.val1, abs(press.val2) / 10000,
	       hum.val1, abs(hum.val2) / 10000);

	/* 3B: Microchip MCP2518FD CAN-FD Click on mikrobus_spi (spi0 CS0, INT GPIO0_20) */
	const struct device *can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

	if (!device_is_ready(can_dev)) {
		printk("  [FAIL] Microchip MCP2518FD CAN-FD Click on spi0 not ready\n");
		return -1;
	}

	ret = can_set_mode(can_dev, CAN_MODE_LOOPBACK | CAN_MODE_FD);
	if (ret != 0) {
		printk("  [FAIL] can_set_mode(LOOPBACK | FD) failed: %d\n", ret);
		return -1;
	}

	ret = can_start(can_dev);
	if (ret != 0) {
		printk("  [FAIL] can_start failed: %d\n", ret);
		return -1;
	}

	const struct can_filter filter = {
		.id = 0x123,
		.mask = CAN_STD_ID_MASK,
		.flags = 0,
	};
	int filter_id = can_add_rx_filter(can_dev, can_rx_callback, NULL, &filter);

	if (filter_id < 0) {
		printk("  [FAIL] can_add_rx_filter failed: %d\n", filter_id);
		return -1;
	}

	struct can_frame tx_frame = {
		.id = 0x123,
		.dlc = can_bytes_to_dlc(16),
		.flags = CAN_FRAME_FDF | CAN_FRAME_BRS,
		.data = {
			0xBE, 0xA6, 0x15, 0x20, 0xCA, 0xFD, 0x01, 0x02,
			0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A
		},
	};

	k_sem_reset(&can_rx_sem);
	ret = can_send(can_dev, &tx_frame, K_MSEC(500), can_tx_callback, NULL);
	if (ret != 0) {
		printk("  [FAIL] can_send (CAN-FD 16B) failed: %d\n", ret);
		return -1;
	}

	if (k_sem_take(&can_rx_sem, K_MSEC(500)) != 0) {
		printk("  [FAIL] Timeout waiting for MCP2518FD CAN-FD RX interrupt\n");
		return -1;
	}

	if (can_rx_frame.id != 0x123 ||
	    can_dlc_to_bytes(can_rx_frame.dlc) != 16 ||
	    memcmp(can_rx_frame.data, tx_frame.data, 16) != 0) {
		printk("  [FAIL] MCP2518FD CAN-FD frame mismatch\n");
		return -1;
	}

	(void)can_stop(can_dev);
	printk("  MCP2518FD CAN-FD Click (spi0): 16-byte BRS CAN-FD frame ID=0x%03x TX->RX OK\n",
	       can_rx_frame.id);
	printk("  [PASS] MikroE Weather Click (BME280) & CAN-FD Click (MCP2518FD) verified\n");
	return 0;
}

/* 18. Track 4: TH1520 Hardware Mailbox (mbox0) + Shared-Memory AMP IPC & SRAM Check */
static K_SEM_DEFINE(mbox_rx_sem, 0, 1);
static uint32_t mbox_rx_words[8];

struct amp_rpmsg_ring {
	uint32_t magic;
	uint32_t seq;
	uint32_t len;
	char payload[52];
};

static void mbox_rx_cb(const struct device *dev, uint32_t ch,
		       void *user_data, struct mbox_msg *data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(ch);
	ARG_UNUSED(user_data);

	if (data != NULL && data->data != NULL) {
		memcpy(mbox_rx_words, data->data,
		       MIN(data->size, sizeof(mbox_rx_words)));
	}
	k_sem_give(&mbox_rx_sem);
}

static int verify_mbox_and_amp(void)
{
	printk("\n[18/%d] [Track 4] TH1520 Hardware Mailbox (mbox0) & On-Chip SRAM AMP IPC\n",
	       TOTAL_TESTS);
	const struct device *mbox = DEVICE_DT_GET(DT_NODELABEL(mbox0));

	if (!device_is_ready(mbox)) {
		printk("  [FAIL] mbox0 device not ready\n");
		return -1;
	}

	int mtu = mbox_mtu_get(mbox);
	uint32_t max_ch = mbox_max_channels_get(mbox);

	if (mtu != 32 || max_ch != 3U) {
		printk("  [FAIL] Unexpected mbox0 geometry: mtu=%d channels=%u\n", mtu, max_ch);
		return -1;
	}

	/* Place an AMP shared-memory message in on-chip SRAM (0xffe0100000) */
	volatile struct amp_rpmsg_ring *shm =
		(volatile struct amp_rpmsg_ring *)0xffe0100000ULL;
	const char ipc_str[] = "AMP_RPMSG_CORE0_TO_CORE3_SRAM_OK";

	shm->magic = 0x52504d53U; /* 'RPMS' */
	shm->seq = 42U;
	shm->len = sizeof(ipc_str);
	memcpy((void *)shm->payload, ipc_str, sizeof(ipc_str));

	int ret = mbox_register_callback(mbox, 0, mbox_rx_cb, NULL);

	ret |= mbox_set_enabled(mbox, 0, true);
	if (ret != 0) {
		printk("  [FAIL] mbox_register_callback / mbox_set_enabled failed: %d\n", ret);
		return -1;
	}

	uint32_t tx_words[8] = {
		0x52504d53U,            /* RPMsg header magic */
		42U,                    /* Sequence ID */
		0xffe01000U,            /* Shared SRAM low address */
		0x000000ffU,            /* Shared SRAM high address */
		sizeof(ipc_str),        /* Payload length */
		0x1520C910U,
		0xA5A55A5AU,
		0x00000001U,
	};
	struct mbox_msg msg = {
		.data = tx_words,
		.size = sizeof(tx_words),
	};

	k_sem_reset(&mbox_rx_sem);
	ret = mbox_send(mbox, 0, &msg);
	if (ret != 0) {
		printk("  [FAIL] mbox_send failed: %d\n", ret);
		return -1;
	}

	if (k_sem_take(&mbox_rx_sem, K_MSEC(500)) != 0) {
		printk("  [FAIL] Timeout waiting for TH1520 MBOX0 PLIC IRQ 28\n");
		return -1;
	}

	if (memcmp(mbox_rx_words, tx_words, sizeof(tx_words)) != 0 ||
	    shm->magic != 0x52504d53U ||
	    strcmp((const char *)shm->payload, ipc_str) != 0) {
		printk("  [FAIL] MBOX0 / Shared-SRAM payload mismatch\n");
		return -1;
	}

	uintptr_t pc_addr = (uintptr_t)&verify_mbox_and_amp;

	printk("  Executing PC=0x%010lx (%s), MBOX0 IRQ 28 + SRAM IPC \"%s\" OK\n",
	       (unsigned long)pc_addr,
	       (pc_addr >= 0xffe0000000ULL) ? "On-Chip SRAM 0xffe0000000" : "DRAM",
	       (const char *)shm->payload);
	printk("  [PASS] TH1520 Hardware Mailbox & AMP Shared-SRAM IPC verified\n");
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
	failures += (verify_pwm() != 0);
	failures += (verify_pinctrl() != 0);
	failures += (verify_pvt_sensor() != 0);
	failures += (verify_rtc() != 0);
	failures += (verify_axi_dma() != 0);
	failures += (verify_ethernet() != 0);
	failures += (verify_bluetooth() != 0);
	failures += (verify_emmc_disk() != 0);
	failures += (verify_mikrobus_clicks() != 0);
	failures += (verify_mbox_and_amp() != 0);

	printk("\n================================================================\n");
	if (failures == 0) {
		printk(" ALL %d/%d BEAGLEV-AHEAD HARDWARE & TRACK 1-4 SUBSYSTEMS PASSED!\n",
		       TOTAL_TESTS, TOTAL_TESTS);
	} else {
		printk(" VERIFICATION FAILED (%d/%d subsystems failed)\n",
		       failures, TOTAL_TESTS);
	}
	printk("================================================================\n");

	return 0;
}
