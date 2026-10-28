/*
 * Copyright (c) 2026 BeagleBoard.org Foundation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT thead_th1520_pinctrl

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/pinctrl/th1520-pinctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(pinctrl_th1520, CONFIG_PINCTRL_LOG_LEVEL);

#define TH1520_PADCFG_OFFSET(pin)	(((pin) / 2U) * 4U)
#define TH1520_PADCFG_SHIFT(pin)	(((pin) & 1U) * 16U)
#define TH1520_PADCFG_MASK		0x3ffU

#define TH1520_MUXCFG_BASE		0x400U
#define TH1520_MUXCFG_OFFSET(pin)	(TH1520_MUXCFG_BASE + (((pin) / 8U) * 4U))
#define TH1520_MUXCFG_SHIFT(pin)	(((pin) % 8U) * 4U)
#define TH1520_MUXCFG_MASK		0xfU

static void pinctrl_th1520_configure_pin(const pinctrl_soc_pin_t *pin)
{
	uint32_t pad = pin->pinmux & 0xffU;
	uint32_t mux = (pin->pinmux >> 8) & TH1520_MUXCFG_MASK;
	uint32_t padcfg = (pin->pinmux >> 16) & TH1520_PADCFG_MASK;
	mm_reg_t pad_reg = pin->base + TH1520_PADCFG_OFFSET(pad);
	mm_reg_t mux_reg = pin->base + TH1520_MUXCFG_OFFSET(pad);
	uint32_t pad_shift = TH1520_PADCFG_SHIFT(pad);
	uint32_t mux_shift = TH1520_MUXCFG_SHIFT(pad);
	uint32_t val;

	val = sys_read32(pad_reg);
	val &= ~(TH1520_PADCFG_MASK << pad_shift);
	val |= (padcfg << pad_shift);
	sys_write32(val, pad_reg);

	val = sys_read32(mux_reg);
	val &= ~(TH1520_MUXCFG_MASK << mux_shift);
	val |= (mux << mux_shift);
	sys_write32(val, mux_reg);
}

int pinctrl_configure_pins(const pinctrl_soc_pin_t *pins, uint8_t pin_cnt,
			   uintptr_t reg)
{
	ARG_UNUSED(reg);

	for (uint8_t i = 0U; i < pin_cnt; i++) {
		pinctrl_th1520_configure_pin(&pins[i]);
	}

	return 0;
}
