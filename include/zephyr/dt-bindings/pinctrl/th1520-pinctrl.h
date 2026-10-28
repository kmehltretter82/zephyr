/*
 * Copyright (c) 2026 BeagleBoard.org Foundation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_TH1520_PINCTRL_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_TH1520_PINCTRL_H_

/*
 * T-Head TH1520 Pinmux encoding:
 *   bits [7:0]   : pin index within the padctrl bank (0..63)
 *   bits [11:8]  : 4-bit function mux selector (0..5)
 *   bits [25:16] : 10-bit PADCFG value (drive strength, pull, Schmitt, IE)
 */
#define TH1520_MUX_FUNC0		0U
#define TH1520_MUX_FUNC1		1U
#define TH1520_MUX_FUNC2		2U
#define TH1520_MUX_FUNC3		3U
#define TH1520_MUX_GPIO			3U
#define TH1520_MUX_FUNC4		4U
#define TH1520_MUX_FUNC5		5U

/* 10-bit PADCFG field definitions */
#define TH1520_PADCFG_DS_MASK		0x00fU
#define TH1520_PADCFG_PULL_DOWN		0x010U
#define TH1520_PADCFG_PULL_UP		0x020U
#define TH1520_PADCFG_SLEW_FAST		0x040U
#define TH1520_PADCFG_SCHMITT_EN	0x080U
#define TH1520_PADCFG_INPUT_EN		0x200U
#define TH1520_PADCFG_DEFAULT		0x208U

#define TH1520_PINMUX(pin, func, padcfg) \
	((((pin) & 0xffU)) | (((func) & 0xfU) << 8) | (((padcfg) & 0x3ffU) << 16))

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_TH1520_PINCTRL_H_ */
