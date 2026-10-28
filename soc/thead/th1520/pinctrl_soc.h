/*
 * Copyright (c) 2026 BeagleBoard.org Foundation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SOC_THEAD_TH1520_PINCTRL_SOC_H_
#define ZEPHYR_SOC_THEAD_TH1520_PINCTRL_SOC_H_

#include <zephyr/devicetree.h>
#include <zephyr/types.h>
#include <zephyr/dt-bindings/pinctrl/th1520-pinctrl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uintptr_t base;
	uint32_t pinmux;
} pinctrl_soc_pin_t;

#define Z_PINCTRL_TH1520_PADCFG(node_id)				\
	((DT_PROP(node_id, input_enable) ? TH1520_PADCFG_INPUT_EN : 0U) | \
	 (DT_PROP(node_id, input_schmitt_enable) ? TH1520_PADCFG_SCHMITT_EN : 0U) | \
	 (DT_PROP(node_id, bias_pull_up) ? TH1520_PADCFG_PULL_UP : 0U) | \
	 (DT_PROP(node_id, bias_pull_down) ? TH1520_PADCFG_PULL_DOWN : 0U) | \
	 (DT_PROP_OR(node_id, drive_strength, 8) & TH1520_PADCFG_DS_MASK))

#define Z_PINCTRL_STATE_PIN_INIT(node_id, prop, idx)			\
	{								\
		.base = DT_REG_ADDR(DT_PARENT(DT_PROP_BY_IDX(node_id, prop, idx))), \
		.pinmux = DT_PROP(DT_PROP_BY_IDX(node_id, prop, idx), pinmux) | \
			  (Z_PINCTRL_TH1520_PADCFG(DT_PROP_BY_IDX(node_id, prop, idx)) << 16), \
	},

#define Z_PINCTRL_STATE_PINS_INIT(node_id, prop)			\
	{ DT_FOREACH_PROP_ELEM(node_id, prop, Z_PINCTRL_STATE_PIN_INIT) }

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_SOC_THEAD_TH1520_PINCTRL_SOC_H_ */
