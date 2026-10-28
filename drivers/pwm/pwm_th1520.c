/*
 * Copyright (c) 2026 BeagleBoard.org Foundation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT thead_th1520_pwm

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#ifdef CONFIG_PINCTRL
#include <zephyr/drivers/pinctrl.h>
#endif
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(pwm_th1520, CONFIG_PWM_LOG_LEVEL);

#define TH1520_PWM_MAX_CHANNELS		6U
#define TH1520_PWM_CH_STRIDE		0x20U
#define TH1520_PWM_CTRL(ch)		((ch) * TH1520_PWM_CH_STRIDE + 0x00U)
#define TH1520_PWM_PERIOD(ch)		((ch) * TH1520_PWM_CH_STRIDE + 0x08U)
#define TH1520_PWM_FP(ch)		((ch) * TH1520_PWM_CH_STRIDE + 0x0cU)

#define TH1520_PWM_CTRL_START		BIT(0)
#define TH1520_PWM_CTRL_CFG_UPDATE	BIT(2)
#define TH1520_PWM_CTRL_CONTINUOUS	BIT(5)
#define TH1520_PWM_CTRL_FPOUT		BIT(8)

struct pwm_th1520_config {
	mm_reg_t base;
	uint32_t clock_freq;
#ifdef CONFIG_PINCTRL
	const struct pinctrl_dev_config *pcfg;
#endif
};

static int pwm_th1520_set_cycles(const struct device *dev, uint32_t channel,
				 uint32_t period_cycles, uint32_t pulse_cycles,
				 pwm_flags_t flags)
{
	const struct pwm_th1520_config *cfg = dev->config;
	uint32_t ctrl;

	if (channel >= TH1520_PWM_MAX_CHANNELS) {
		return -EINVAL;
	}

	if (pulse_cycles > period_cycles) {
		return -EINVAL;
	}

	if (period_cycles == 0U) {
		sys_write32(0U, cfg->base + TH1520_PWM_CTRL(channel));
		sys_write32(0U, cfg->base + TH1520_PWM_PERIOD(channel));
		sys_write32(0U, cfg->base + TH1520_PWM_FP(channel));
		return 0;
	}

	ctrl = TH1520_PWM_CTRL_CONTINUOUS;
	if ((flags & PWM_POLARITY_MASK) != PWM_POLARITY_INVERTED) {
		ctrl |= TH1520_PWM_CTRL_FPOUT;
	}

	sys_write32(ctrl, cfg->base + TH1520_PWM_CTRL(channel));
	sys_write32(period_cycles, cfg->base + TH1520_PWM_PERIOD(channel));
	sys_write32(pulse_cycles, cfg->base + TH1520_PWM_FP(channel));
	sys_write32(ctrl | TH1520_PWM_CTRL_CFG_UPDATE,
		    cfg->base + TH1520_PWM_CTRL(channel));
	sys_write32(ctrl | TH1520_PWM_CTRL_START,
		    cfg->base + TH1520_PWM_CTRL(channel));

	return 0;
}

static int pwm_th1520_get_cycles_per_sec(const struct device *dev,
					 uint32_t channel, uint64_t *cycles)
{
	const struct pwm_th1520_config *cfg = dev->config;

	if (channel >= TH1520_PWM_MAX_CHANNELS || cycles == NULL) {
		return -EINVAL;
	}

	*cycles = cfg->clock_freq;
	return 0;
}

static DEVICE_API(pwm, pwm_th1520_driver_api) = {
	.set_cycles = pwm_th1520_set_cycles,
	.get_cycles_per_sec = pwm_th1520_get_cycles_per_sec,
};

static int pwm_th1520_init(const struct device *dev)
{
#ifdef CONFIG_PINCTRL
	const struct pwm_th1520_config *cfg = dev->config;
	int ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);

	if (ret < 0 && ret != -ENOENT) {
		return ret;
	}
#else
	ARG_UNUSED(dev);
#endif
	return 0;
}

#ifdef CONFIG_PINCTRL
#define PWM_TH1520_PINCTRL_DEFINE(inst) PINCTRL_DT_INST_DEFINE(inst);
#define PWM_TH1520_PINCTRL_INIT(inst) .pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),
#else
#define PWM_TH1520_PINCTRL_DEFINE(inst)
#define PWM_TH1520_PINCTRL_INIT(inst)
#endif

#define PWM_TH1520_INIT(inst)						\
	PWM_TH1520_PINCTRL_DEFINE(inst)					\
	static const struct pwm_th1520_config pwm_th1520_cfg_##inst = {	\
		.base = DT_INST_REG_ADDR(inst),				\
		.clock_freq = DT_INST_PROP(inst, clock_frequency),	\
		PWM_TH1520_PINCTRL_INIT(inst)				\
	};								\
	DEVICE_DT_INST_DEFINE(inst, pwm_th1520_init, NULL, NULL,	\
			      &pwm_th1520_cfg_##inst, POST_KERNEL,	\
			      CONFIG_PWM_INIT_PRIORITY,			\
			      &pwm_th1520_driver_api);

DT_INST_FOREACH_STATUS_OKAY(PWM_TH1520_INIT)
