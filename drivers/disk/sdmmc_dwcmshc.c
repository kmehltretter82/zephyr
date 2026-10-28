/*
 * Copyright (c) 2026 BeagleBoard.org Foundation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT thead_th1520_dwcmshc

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/disk.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(sdmmc_dwcmshc, CONFIG_LOG_DEFAULT_LEVEL);

/* Standard SDHCI register offsets */
#define SDHC_SYSAD			0x00U
#define SDHC_BLKSIZE			0x04U
#define SDHC_BLKCNT			0x06U
#define SDHC_ARGUMENT			0x08U
#define SDHC_TRNMOD			0x0cU
#define SDHC_CMDREG			0x0eU
#define SDHC_RSPREG0			0x10U
#define SDHC_RSPREG1			0x14U
#define SDHC_RSPREG2			0x18U
#define SDHC_RSPREG3			0x1cU
#define SDHC_BDATA			0x20U
#define SDHC_PRNSTS			0x24U
#define SDHC_HOSTCTL			0x28U
#define SDHC_PWRCON			0x29U
#define SDHC_CLKCON			0x2cU
#define SDHC_TIMEOUTCON			0x2eU
#define SDHC_SWRST			0x2fU
#define SDHC_NORINTSTS			0x30U
#define SDHC_ERRINTSTS			0x32U
#define SDHC_NORINTSTSEN		0x34U
#define SDHC_ERRINTSTSEN		0x36U

/* DWC MSHC vendor/PHY registers */
#define DWCMSHC_PHY_CNFG		0x300U
#define DWCMSHC_PHY_DLL_CTRL		0x324U
#define DWCMSHC_PHY_DLL_STATUS		0x32eU
#define DWCMSHC_EMMC_CTRL		0x52cU

#define DWCMSHC_PHY_RSTN		BIT(0)
#define DWCMSHC_DLL_ENABLE		BIT(0)

/* SDHCI bit masks */
#define SDHC_TRNS_BLK_CNT_EN		BIT(1)
#define SDHC_TRNS_READ			BIT(4)
#define SDHC_CMD_RSP_48			0x02U
#define SDHC_CMD_RSP_136		0x01U
#define SDHC_CMD_RSP_48_BUSY		0x03U
#define SDHC_CMD_DATA_PRESENT		BIT(5)

#define SDHC_PRNSTS_CARD_PRESENT	BIT(16)
#define SDHC_PWRCON_ON_3V3		(0x01U | (0x07U << 1))
#define SDHC_CLKCON_DEFAULT		0x0007U
#define SDHC_SWRST_ALL			0x01U

#define DWCMSHC_SECTOR_SIZE		512U
#define DWCMSHC_DEFAULT_SECTORS		4096U /* 2 MiB default backing */

struct dwcmshc_config {
	mm_reg_t base;
};

struct dwcmshc_data {
	struct disk_info *info;
	struct k_mutex lock;
	uint32_t ocr;
	uint32_t cid[4];
	uint32_t sector_count;
	uint8_t ext_csd_rev;
	bool card_ready;
};

static void dwcmshc_send_cmd(mm_reg_t base, uint16_t blksize, uint16_t blkcnt,
			     uint32_t arg, uint16_t trnmod, uint16_t cmd)
{
	sys_write16(0xffffU, base + SDHC_NORINTSTS);
	sys_write16(0xffffU, base + SDHC_ERRINTSTS);
	sys_write16(blksize, base + SDHC_BLKSIZE);
	sys_write16(blkcnt, base + SDHC_BLKCNT);
	sys_write32(arg, base + SDHC_ARGUMENT);
	sys_write16(trnmod, base + SDHC_TRNMOD);
	sys_write16(cmd, base + SDHC_CMDREG);
}

static int dwcmshc_card_init(const struct device *dev)
{
	const struct dwcmshc_config *cfg = dev->config;
	struct dwcmshc_data *data = dev->data;
	mm_reg_t base = cfg->base;
	uint8_t ext_csd[DWCMSHC_SECTOR_SIZE];
	const uint32_t rca = 1U;

	if (data->card_ready) {
		return 0;
	}

	/* Reset controller and enable power + clock */
	sys_write8(SDHC_SWRST_ALL, base + SDHC_SWRST);
	sys_write8(SDHC_PWRCON_ON_3V3, base + SDHC_PWRCON);
	sys_write16(SDHC_CLKCON_DEFAULT, base + SDHC_CLKCON);
	sys_write16(0x00ffU, base + SDHC_NORINTSTSEN);
	sys_write16(0x03ffU, base + SDHC_ERRINTSTSEN);

	/* Release DWC MSHC PHY reset and enable DLL */
	sys_write8(DWCMSHC_PHY_RSTN, base + DWCMSHC_PHY_CNFG);
	sys_write8(DWCMSHC_DLL_ENABLE, base + DWCMSHC_PHY_DLL_CTRL);
	sys_write16(0x0001U, base + DWCMSHC_EMMC_CTRL);

	if (!(sys_read32(base + SDHC_PRNSTS) & SDHC_PRNSTS_CARD_PRESENT)) {
		LOG_WRN("No eMMC/SD medium present on DWCMSHC @ 0x%lx",
			(unsigned long)base);
		return -ENODEV;
	}

	/* CMD0: GO_IDLE_STATE */
	dwcmshc_send_cmd(base, 0U, 0U, 0U, 0U, 0U << 8);

	/* CMD1: SEND_OP_COND (eMMC sector/byte mode) */
	dwcmshc_send_cmd(base, 0U, 0U, 0x40ff8080U, 0U,
			 (1U << 8) | SDHC_CMD_RSP_48);
	data->ocr = sys_read32(base + SDHC_RSPREG0);

	/* CMD2: ALL_SEND_CID */
	dwcmshc_send_cmd(base, 0U, 0U, 0U, 0U,
			 (2U << 8) | SDHC_CMD_RSP_136);
	data->cid[0] = sys_read32(base + SDHC_RSPREG0);
	data->cid[1] = sys_read32(base + SDHC_RSPREG1);
	data->cid[2] = sys_read32(base + SDHC_RSPREG2);
	data->cid[3] = sys_read32(base + SDHC_RSPREG3);

	/* CMD3: SET_RELATIVE_ADDR */
	dwcmshc_send_cmd(base, 0U, 0U, rca << 16, 0U,
			 (3U << 8) | SDHC_CMD_RSP_48);

	/* CMD7: SELECT_CARD */
	dwcmshc_send_cmd(base, 0U, 0U, rca << 16, 0U,
			 (7U << 8) | SDHC_CMD_RSP_48_BUSY);

	/* CMD8: SEND_EXT_CSD (512 bytes) */
	dwcmshc_send_cmd(base, DWCMSHC_SECTOR_SIZE, 1U, 0U,
			 SDHC_TRNS_READ | SDHC_TRNS_BLK_CNT_EN,
			 (8U << 8) | SDHC_CMD_RSP_48 | SDHC_CMD_DATA_PRESENT);
	for (size_t i = 0; i < DWCMSHC_SECTOR_SIZE; i += sizeof(uint32_t)) {
		uint32_t word = sys_read32(base + SDHC_BDATA);

		sys_put_le32(word, &ext_csd[i]);
	}

	data->ext_csd_rev = ext_csd[192];
	uint32_t sec_count = sys_get_le32(&ext_csd[212]);

	data->sector_count = (sec_count != 0U) ? sec_count : DWCMSHC_DEFAULT_SECTORS;
	data->card_ready = true;

	LOG_INF("TH1520 DWCMSHC eMMC initialized: OCR=0x%08x EXT_CSD_REV=%u sectors=%u",
		data->ocr, data->ext_csd_rev, data->sector_count);
	return 0;
}

static int dwcmshc_disk_status(struct disk_info *disk)
{
	struct dwcmshc_data *data = disk->dev->data;

	return data->card_ready ? DISK_STATUS_OK : DISK_STATUS_UNINIT;
}

static int dwcmshc_disk_read(struct disk_info *disk, uint8_t *buff,
			     uint32_t sector, uint32_t count)
{
	const struct dwcmshc_config *cfg = disk->dev->config;
	struct dwcmshc_data *data = disk->dev->data;
	mm_reg_t base = cfg->base;

	if (!data->card_ready) {
		int ret = dwcmshc_card_init(disk->dev);

		if (ret < 0) {
			return ret;
		}
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	for (uint32_t b = 0; b < count; b++) {
		uint32_t cur_sec = sector + b;
		uint32_t arg = (data->ocr & BIT(30)) ?
			       cur_sec : (cur_sec * DWCMSHC_SECTOR_SIZE);
		uint8_t *dst = buff + (size_t)b * DWCMSHC_SECTOR_SIZE;

		/* CMD17: READ_SINGLE_BLOCK */
		dwcmshc_send_cmd(base, DWCMSHC_SECTOR_SIZE, 1U, arg,
				 SDHC_TRNS_READ | SDHC_TRNS_BLK_CNT_EN,
				 (17U << 8) | SDHC_CMD_RSP_48 |
				 SDHC_CMD_DATA_PRESENT);

		for (size_t i = 0; i < DWCMSHC_SECTOR_SIZE; i += sizeof(uint32_t)) {
			uint32_t word = sys_read32(base + SDHC_BDATA);

			sys_put_le32(word, &dst[i]);
		}
	}

	k_mutex_unlock(&data->lock);
	return 0;
}

static int dwcmshc_disk_write(struct disk_info *disk, const uint8_t *buff,
			      uint32_t sector, uint32_t count)
{
	const struct dwcmshc_config *cfg = disk->dev->config;
	struct dwcmshc_data *data = disk->dev->data;
	mm_reg_t base = cfg->base;

	if (!data->card_ready) {
		int ret = dwcmshc_card_init(disk->dev);

		if (ret < 0) {
			return ret;
		}
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	for (uint32_t b = 0; b < count; b++) {
		uint32_t cur_sec = sector + b;
		uint32_t arg = (data->ocr & BIT(30)) ?
			       cur_sec : (cur_sec * DWCMSHC_SECTOR_SIZE);
		const uint8_t *src = buff + (size_t)b * DWCMSHC_SECTOR_SIZE;

		/* CMD24: WRITE_BLOCK */
		dwcmshc_send_cmd(base, DWCMSHC_SECTOR_SIZE, 1U, arg,
				 SDHC_TRNS_BLK_CNT_EN,
				 (24U << 8) | SDHC_CMD_RSP_48 |
				 SDHC_CMD_DATA_PRESENT);

		for (size_t i = 0; i < DWCMSHC_SECTOR_SIZE; i += sizeof(uint32_t)) {
			sys_write32(sys_get_le32(&src[i]), base + SDHC_BDATA);
		}
	}

	k_mutex_unlock(&data->lock);
	return 0;
}

static int dwcmshc_disk_ioctl(struct disk_info *disk, uint8_t cmd, void *buff)
{
	struct dwcmshc_data *data = disk->dev->data;

	switch (cmd) {
	case DISK_IOCTL_CTRL_INIT:
		return dwcmshc_card_init(disk->dev);
	case DISK_IOCTL_CTRL_DEINIT:
	case DISK_IOCTL_CTRL_SYNC:
		return 0;
	case DISK_IOCTL_GET_SECTOR_COUNT:
		if (buff == NULL) {
			return -EINVAL;
		}
		*(uint32_t *)buff = data->sector_count;
		return 0;
	case DISK_IOCTL_GET_SECTOR_SIZE:
		if (buff == NULL) {
			return -EINVAL;
		}
		*(uint32_t *)buff = DWCMSHC_SECTOR_SIZE;
		return 0;
	case DISK_IOCTL_GET_ERASE_BLOCK_SZ:
		if (buff == NULL) {
			return -EINVAL;
		}
		*(uint32_t *)buff = 1U;
		return 0;
	default:
		return -EINVAL;
	}
}

static int dwcmshc_disk_init(struct disk_info *disk)
{
	return dwcmshc_disk_ioctl(disk, DISK_IOCTL_CTRL_INIT, NULL);
}

static const struct disk_operations dwcmshc_disk_ops = {
	.init = dwcmshc_disk_init,
	.status = dwcmshc_disk_status,
	.read = dwcmshc_disk_read,
	.write = dwcmshc_disk_write,
	.ioctl = dwcmshc_disk_ioctl,
};

static int dwcmshc_init(const struct device *dev)
{
	struct dwcmshc_data *data = dev->data;

	k_mutex_init(&data->lock);
	data->info->dev = dev;
	(void)dwcmshc_card_init(dev);
	return disk_access_register(data->info);
}

#define DWCMSHC_INIT(inst)						\
	static struct disk_info dwcmshc_disk_info_##inst = {		\
		.name = DT_INST_PROP_OR(inst, disk_name, "EMMC"),	\
		.ops = &dwcmshc_disk_ops,				\
	};								\
	static struct dwcmshc_data dwcmshc_data_##inst = {		\
		.info = &dwcmshc_disk_info_##inst,			\
		.sector_count = DWCMSHC_DEFAULT_SECTORS,		\
	};								\
	static const struct dwcmshc_config dwcmshc_cfg_##inst = {	\
		.base = DT_INST_REG_ADDR(inst),				\
	};								\
	DEVICE_DT_INST_DEFINE(inst, dwcmshc_init, NULL,			\
			      &dwcmshc_data_##inst,			\
			      &dwcmshc_cfg_##inst, POST_KERNEL,		\
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE,	\
			      &dwcmshc_disk_ops);

DT_INST_FOREACH_STATUS_OKAY(DWCMSHC_INIT)
