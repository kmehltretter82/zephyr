/*
 * Copyright (c) 2026 BeagleBoard.org Foundation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT moortec_mr75203

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(mr75203, CONFIG_SENSOR_LOG_LEVEL);

/* Common window offsets */
#define PVT_COMP_ID		0x00U
#define PVT_IP_CONFIG		0x04U
#define PVT_REG_LOCK		0x10U
#define PVT_UNLOCK_VALUE	0x1acce551U

/* Macro common offsets (TS, PD, VM) */
#define CLK_SYNTH		0x00U
#define CLK_SYNTH_ENABLE	BIT(24)
#define SDIF_DISABLE		0x04U
#define SDIF_STATUS		0x08U
#define SDIF_W			0x0cU
#define SDIF_PROG		BIT(31)
#define SDIF_WRITE		BIT(27)

/* IP_CTRL bits written via SDIF_W to internal addr 0 */
#define IP_CTRL_RESET_RELEASE	BIT(1)
#define IP_CTRL_RUN_CONTINUOUS	BIT(3)
#define IP_CTRL_AUTO		BIT(8)
#define IP_CTRL_VM_MODE		BIT(10)

/* TS / PD per-instance offsets (base + 0x40 + ch * 0x40) */
#define MACRO_COMMON_SIZE	0x40U
#define MACRO_STRIDE		0x40U
#define MACRO_SDIF_DONE(ch)	(MACRO_COMMON_SIZE + (ch) * MACRO_STRIDE + 0x14U)
#define MACRO_SDIF_DATA(ch)	(MACRO_COMMON_SIZE + (ch) * MACRO_STRIDE + 0x18U)

/* VM per-instance offsets (base + 0x200 + vm * 0x200) */
#define VM_COMMON_SIZE		0x200U
#define VM_STRIDE		0x200U
#define VM_SDIF_DONE(vm)	(VM_COMMON_SIZE + (vm) * VM_STRIDE + 0x34U)
#define VM_SDIF_DATA(vm, ch)	(VM_COMMON_SIZE + (vm) * VM_STRIDE + 0x40U + (ch) * 4U)

/* Default Moortec Series 3 temperature calibration coefficients */
#define MR75203_COEFF_G		60000LL
#define MR75203_COEFF_H		200000LL
#define MR75203_COEFF_CAL5	4094LL
#define MR75203_COEFF_J		(-100LL)
#define MR75203_IP_FREQ_MHZ	4LL

struct mr75203_config {
	mm_reg_t cmn_base;
	mm_reg_t ts_base;
	mm_reg_t pd_base;
	mm_reg_t vm_base;
};

struct mr75203_data {
	int32_t temp_mc;
	int32_t volt_mv;
};

static int mr75203_sample_fetch(const struct device *dev,
				enum sensor_channel chan)
{
	const struct mr75203_config *cfg = dev->config;
	struct mr75203_data *data = dev->data;

	if (chan != SENSOR_CHAN_ALL &&
	    chan != SENSOR_CHAN_DIE_TEMP &&
	    chan != SENSOR_CHAN_AMBIENT_TEMP &&
	    chan != SENSOR_CHAN_VOLTAGE) {
		return -ENOTSUP;
	}

	if (chan == SENSOR_CHAN_ALL ||
	    chan == SENSOR_CHAN_DIE_TEMP ||
	    chan == SENSOR_CHAN_AMBIENT_TEMP) {
		uint32_t raw = sys_read32(cfg->ts_base + MACRO_SDIF_DATA(0)) & 0xfffU;
		int64_t mc = MR75203_COEFF_G +
			     (MR75203_COEFF_H * (int64_t)raw) / MR75203_COEFF_CAL5 -
			     (MR75203_COEFF_H / 2) +
			     (MR75203_COEFF_J * MR75203_IP_FREQ_MHZ);

		data->temp_mc = (int32_t)mc;
	}

	if (chan == SENSOR_CHAN_ALL || chan == SENSOR_CHAN_VOLTAGE) {
		uint32_t raw = sys_read32(cfg->vm_base + VM_SDIF_DATA(0, 0)) & 0x3fffU;
		int64_t mv = (90LL * (int64_t)raw - 245805LL) / 1024LL;

		data->volt_mv = (int32_t)mv;
	}

	return 0;
}

static int mr75203_channel_get(const struct device *dev,
			       enum sensor_channel chan,
			       struct sensor_value *val)
{
	const struct mr75203_data *data = dev->data;

	switch (chan) {
	case SENSOR_CHAN_DIE_TEMP:
	case SENSOR_CHAN_AMBIENT_TEMP:
		val->val1 = data->temp_mc / 1000;
		val->val2 = (data->temp_mc % 1000) * 1000;
		return 0;
	case SENSOR_CHAN_VOLTAGE:
		val->val1 = data->volt_mv / 1000;
		val->val2 = (data->volt_mv % 1000) * 1000;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static DEVICE_API(sensor, mr75203_driver_api) = {
	.sample_fetch = mr75203_sample_fetch,
	.channel_get = mr75203_channel_get,
};

static int mr75203_init(const struct device *dev)
{
	const struct mr75203_config *cfg = dev->config;
	uint32_t clk_synth = CLK_SYNTH_ENABLE | (2U << 8) | 2U; /* 24 MHz / 6 = 4 MHz */
	uint32_t ts_ctrl = SDIF_PROG | SDIF_WRITE |
			   IP_CTRL_RESET_RELEASE | IP_CTRL_RUN_CONTINUOUS |
			   IP_CTRL_AUTO;
	uint32_t vm_ctrl = ts_ctrl | IP_CTRL_VM_MODE;

	/* Unlock PVT register write access */
	sys_write32(PVT_UNLOCK_VALUE, cfg->cmn_base + PVT_REG_LOCK);

	/* Enable TS macro0 continuous auto-sampling */
	sys_write32(clk_synth, cfg->ts_base + CLK_SYNTH);
	sys_write32(0U, cfg->ts_base + SDIF_DISABLE);
	sys_write32(ts_ctrl, cfg->ts_base + SDIF_W);

	/* Enable VM macro0 continuous auto-sampling */
	sys_write32(clk_synth, cfg->vm_base + CLK_SYNTH);
	sys_write32(0U, cfg->vm_base + SDIF_DISABLE);
	sys_write32(vm_ctrl, cfg->vm_base + SDIF_W);

	return 0;
}

#define MR75203_INIT(inst)						\
	static struct mr75203_data mr75203_data_##inst;			\
	static const struct mr75203_config mr75203_cfg_##inst = {	\
		.cmn_base = DT_INST_REG_ADDR_BY_IDX(inst, 0),		\
		.ts_base = DT_INST_REG_ADDR_BY_IDX(inst, 1),		\
		.pd_base = DT_INST_REG_ADDR_BY_IDX(inst, 2),		\
		.vm_base = DT_INST_REG_ADDR_BY_IDX(inst, 3),		\
	};								\
	SENSOR_DEVICE_DT_INST_DEFINE(inst, mr75203_init, NULL,		\
				     &mr75203_data_##inst,		\
				     &mr75203_cfg_##inst, POST_KERNEL,	\
				     CONFIG_SENSOR_INIT_PRIORITY,	\
				     &mr75203_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MR75203_INIT)
