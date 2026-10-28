/*
 * Copyright (c) 2026 BeagleBoard.org Foundation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT apm_xgene_rtc

#include <errno.h>
#include <time.h>
#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/timeutil.h>
#include <zephyr/sys/util.h>
#include "rtc_utils.h"

LOG_MODULE_REGISTER(rtc_xgene, CONFIG_RTC_LOG_LEVEL);

#define XGENE_RTC_CCVR		0x00U
#define XGENE_RTC_CMR		0x04U
#define XGENE_RTC_CLR		0x08U
#define XGENE_RTC_CCR		0x0cU
#define XGENE_RTC_STAT		0x10U
#define XGENE_RTC_RSTAT		0x14U
#define XGENE_RTC_EOI		0x18U
#define XGENE_RTC_VER		0x1cU
#define XGENE_RTC_CPSR		0x20U
#define XGENE_RTC_CPCVR		0x24U

#define XGENE_RTC_CCR_IE	BIT(0)
#define XGENE_RTC_CCR_MASK	BIT(1)
#define XGENE_RTC_CCR_EN	BIT(2)
#define XGENE_RTC_CCR_WEN	BIT(3)
#define XGENE_RTC_CCR_PSCLR_EN	BIT(4)

#define XGENE_RTC_PRESCALER	32768U

#define XGENE_RTC_TIME_MASK	(RTC_ALARM_TIME_MASK_SECOND |	\
				 RTC_ALARM_TIME_MASK_MINUTE |	\
				 RTC_ALARM_TIME_MASK_HOUR |	\
				 RTC_ALARM_TIME_MASK_MONTHDAY |	\
				 RTC_ALARM_TIME_MASK_MONTH |	\
				 RTC_ALARM_TIME_MASK_YEAR)

struct rtc_xgene_config {
	mm_reg_t base;
};

struct rtc_xgene_data {
	struct k_spinlock lock;
};

static int rtc_xgene_set_time(const struct device *dev,
			      const struct rtc_time *timeptr)
{
	const struct rtc_xgene_config *cfg = dev->config;
	struct rtc_xgene_data *data = dev->data;
	int64_t sec;

	if (timeptr == NULL ||
	    !rtc_utils_validate_rtc_time(timeptr, XGENE_RTC_TIME_MASK)) {
		return -EINVAL;
	}

	sec = timeutil_timegm64(rtc_time_to_tm((struct rtc_time *)timeptr));
	if (sec < 0 || sec > UINT32_MAX) {
		return -EINVAL;
	}

	K_SPINLOCK(&data->lock) {
		sys_write32(0U, cfg->base + XGENE_RTC_CCR);
		sys_write32(XGENE_RTC_PRESCALER, cfg->base + XGENE_RTC_CPSR);
		sys_write32((uint32_t)sec, cfg->base + XGENE_RTC_CLR);
		sys_write32(XGENE_RTC_CCR_EN | XGENE_RTC_CCR_PSCLR_EN,
			    cfg->base + XGENE_RTC_CCR);
	}

	return 0;
}

static int rtc_xgene_get_time(const struct device *dev,
			      struct rtc_time *timeptr)
{
	const struct rtc_xgene_config *cfg = dev->config;
	time_t sec;
	struct tm tm_val;

	if (timeptr == NULL) {
		return -EINVAL;
	}

	sec = (time_t)sys_read32(cfg->base + XGENE_RTC_CCVR);
	if (gmtime_r(&sec, &tm_val) == NULL) {
		return -EINVAL;
	}

	memset(timeptr, 0, sizeof(*timeptr));
	timeptr->tm_sec = tm_val.tm_sec;
	timeptr->tm_min = tm_val.tm_min;
	timeptr->tm_hour = tm_val.tm_hour;
	timeptr->tm_mday = tm_val.tm_mday;
	timeptr->tm_mon = tm_val.tm_mon;
	timeptr->tm_year = tm_val.tm_year;
	timeptr->tm_wday = tm_val.tm_wday;
	timeptr->tm_yday = tm_val.tm_yday;
	timeptr->tm_isdst = -1;

	return 0;
}

static DEVICE_API(rtc, rtc_xgene_driver_api) = {
	.set_time = rtc_xgene_set_time,
	.get_time = rtc_xgene_get_time,
};

static int rtc_xgene_init(const struct device *dev)
{
	const struct rtc_xgene_config *cfg = dev->config;

	sys_write32(XGENE_RTC_PRESCALER, cfg->base + XGENE_RTC_CPSR);
	sys_write32(XGENE_RTC_CCR_EN | XGENE_RTC_CCR_PSCLR_EN,
		    cfg->base + XGENE_RTC_CCR);
	(void)sys_read32(cfg->base + XGENE_RTC_EOI);

	return 0;
}

#define RTC_XGENE_INIT(inst)						\
	static struct rtc_xgene_data rtc_xgene_data_##inst;		\
	static const struct rtc_xgene_config rtc_xgene_cfg_##inst = {	\
		.base = DT_INST_REG_ADDR(inst),				\
	};								\
	DEVICE_DT_INST_DEFINE(inst, rtc_xgene_init, NULL,		\
			      &rtc_xgene_data_##inst,			\
			      &rtc_xgene_cfg_##inst, POST_KERNEL,	\
			      CONFIG_RTC_INIT_PRIORITY,			\
			      &rtc_xgene_driver_api);

DT_INST_FOREACH_STATUS_OKAY(RTC_XGENE_INIT)
