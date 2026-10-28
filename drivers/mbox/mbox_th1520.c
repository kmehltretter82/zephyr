/*
 * Copyright (c) 2026 BeagleBoard.org Foundation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT thead_th1520_mbox

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(mbox_th1520, CONFIG_MBOX_LOG_LEVEL);

#define TH1520_MBOX_MAX_CHANNELS	3U
#define TH1520_MBOX_CHANNEL_STRIDE	0x1000U
#define TH1520_MBOX_REMOTE0_OFFSET	0x4000U
#define TH1520_MBOX_INFO_WORDS		8U
#define TH1520_MBOX_MTU			(TH1520_MBOX_INFO_WORDS * sizeof(uint32_t))

#define TH1520_MBOX_STATUS		0x000U
#define TH1520_MBOX_CLEAR		0x004U
#define TH1520_MBOX_MASK		0x00cU
#define TH1520_MBOX_GENERATE		0x010U
#define TH1520_MBOX_INFO0		0x014U
#define TH1520_MBOX_INFO7		0x030U

struct mbox_th1520_config {
	mm_reg_t local_base;
	mm_reg_t remote_base[TH1520_MBOX_MAX_CHANNELS];
	void (*irq_config)(const struct device *dev);
};

struct mbox_th1520_data {
	struct k_spinlock lock;
	mbox_callback_t cb[TH1520_MBOX_MAX_CHANNELS];
	void *user_data[TH1520_MBOX_MAX_CHANNELS];
};

static mm_reg_t mbox_th1520_remote_chan_base(const struct mbox_th1520_config *cfg,
					     uint32_t ch)
{
	if (ch == 0U) {
		return cfg->remote_base[0] + TH1520_MBOX_REMOTE0_OFFSET;
	}
	return cfg->remote_base[ch];
}

static void mbox_th1520_isr(const struct device *dev)
{
	const struct mbox_th1520_config *cfg = dev->config;
	struct mbox_th1520_data *data = dev->data;
	uint32_t status = sys_read32(cfg->local_base + TH1520_MBOX_STATUS);
	uint32_t mask = sys_read32(cfg->local_base + TH1520_MBOX_MASK);
	uint32_t pending = status & mask;

	for (uint32_t ch = 0U; ch < TH1520_MBOX_MAX_CHANNELS; ch++) {
		if (!(pending & BIT(ch))) {
			continue;
		}

		mm_reg_t ch_base = cfg->local_base +
				   (ch + 1U) * TH1520_MBOX_CHANNEL_STRIDE;
		uint32_t payload[TH1520_MBOX_INFO_WORDS];

		for (uint32_t w = 0U; w < TH1520_MBOX_INFO_WORDS; w++) {
			payload[w] = sys_read32(ch_base + TH1520_MBOX_INFO0 + w * 4U);
		}

		/* Clear interrupt status bit and acknowledge INFO7 */
		sys_write32(BIT(ch), cfg->local_base + TH1520_MBOX_CLEAR);
		sys_write32(0U, ch_base + TH1520_MBOX_GENERATE);

		if (data->cb[ch] != NULL) {
			struct mbox_msg msg = {
				.data = payload,
				.size = sizeof(payload),
			};

			data->cb[ch](dev, ch, data->user_data[ch], &msg);
		}
	}
}

static int mbox_th1520_send(const struct device *dev, uint32_t ch,
			    const struct mbox_msg *msg)
{
	const struct mbox_th1520_config *cfg = dev->config;

	if (ch >= TH1520_MBOX_MAX_CHANNELS) {
		return -EINVAL;
	}

	if (msg != NULL && msg->size > TH1520_MBOX_MTU) {
		return -EMSGSIZE;
	}

	mm_reg_t rem_base = mbox_th1520_remote_chan_base(cfg, ch);
	uint32_t words[TH1520_MBOX_INFO_WORDS] = { 0 };

	if (msg != NULL && msg->data != NULL && msg->size > 0U) {
		memcpy(words, msg->data, msg->size);
	}

	for (uint32_t w = 0U; w < TH1520_MBOX_INFO_WORDS; w++) {
		sys_write32(words[w], rem_base + TH1520_MBOX_INFO0 + w * 4U);
	}

	/* Trigger remote/loopback interrupt via GENERATE bit 0 */
	sys_write32(BIT(0), rem_base + TH1520_MBOX_GENERATE);
	return 0;
}

static int mbox_th1520_register_callback(const struct device *dev, uint32_t ch,
					 mbox_callback_t cb, void *user_data)
{
	struct mbox_th1520_data *data = dev->data;

	if (ch >= TH1520_MBOX_MAX_CHANNELS) {
		return -EINVAL;
	}

	K_SPINLOCK(&data->lock) {
		data->cb[ch] = cb;
		data->user_data[ch] = user_data;
	}

	return 0;
}

static int mbox_th1520_mtu_get(const struct device *dev)
{
	ARG_UNUSED(dev);
	return TH1520_MBOX_MTU;
}

static uint32_t mbox_th1520_max_channels_get(const struct device *dev)
{
	ARG_UNUSED(dev);
	return TH1520_MBOX_MAX_CHANNELS;
}

static int mbox_th1520_set_enabled(const struct device *dev, uint32_t ch,
				   bool enable)
{
	const struct mbox_th1520_config *cfg = dev->config;
	struct mbox_th1520_data *data = dev->data;

	if (ch >= TH1520_MBOX_MAX_CHANNELS) {
		return -EINVAL;
	}

	K_SPINLOCK(&data->lock) {
		uint32_t mask = sys_read32(cfg->local_base + TH1520_MBOX_MASK);

		if (enable) {
			mask |= BIT(ch);
		} else {
			mask &= ~BIT(ch);
		}
		sys_write32(mask, cfg->local_base + TH1520_MBOX_MASK);
	}

	return 0;
}

static DEVICE_API(mbox, mbox_th1520_driver_api) = {
	.send = mbox_th1520_send,
	.register_callback = mbox_th1520_register_callback,
	.mtu_get = mbox_th1520_mtu_get,
	.max_channels_get = mbox_th1520_max_channels_get,
	.set_enabled = mbox_th1520_set_enabled,
};

static int mbox_th1520_init(const struct device *dev)
{
	const struct mbox_th1520_config *cfg = dev->config;

	sys_write32(0U, cfg->local_base + TH1520_MBOX_MASK);
	sys_write32(0x7U, cfg->local_base + TH1520_MBOX_CLEAR);
	cfg->irq_config(dev);

	return 0;
}

#define MBOX_TH1520_INIT(inst)						\
	static void mbox_th1520_irq_cfg_##inst(const struct device *dev)\
	{								\
		ARG_UNUSED(dev);					\
		IRQ_CONNECT(DT_INST_IRQN(inst),				\
			    DT_INST_IRQ(inst, priority),		\
			    mbox_th1520_isr,				\
			    DEVICE_DT_INST_GET(inst), 0);		\
		irq_enable(DT_INST_IRQN(inst));				\
	}								\
	static struct mbox_th1520_data mbox_th1520_data_##inst;		\
	static const struct mbox_th1520_config mbox_th1520_cfg_##inst = {\
		.local_base = DT_INST_REG_ADDR_BY_IDX(inst, 0),		\
		.remote_base = {					\
			DT_INST_REG_ADDR_BY_IDX(inst, 1),		\
			DT_INST_REG_ADDR_BY_IDX(inst, 2),		\
			DT_INST_REG_ADDR_BY_IDX(inst, 3),		\
		},							\
		.irq_config = mbox_th1520_irq_cfg_##inst,		\
	};								\
	DEVICE_DT_INST_DEFINE(inst, mbox_th1520_init, NULL,		\
			      &mbox_th1520_data_##inst,			\
			      &mbox_th1520_cfg_##inst, POST_KERNEL,	\
			      CONFIG_MBOX_INIT_PRIORITY,		\
			      &mbox_th1520_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MBOX_TH1520_INIT)
