/*
 * Copyright (c) 2026 BeagleV-Ahead Zephyr Port
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SOC_THEAD_TH1520_SOC_H_
#define ZEPHYR_SOC_THEAD_TH1520_SOC_H_

#ifndef _ASMLANGUAGE
#include <zephyr/types.h>

/* T-Head XuanTie C910 vendor CSR numbers */
#define TH1520_CSR_MXSTATUS   0x7c0
#define TH1520_CSR_MHCR       0x7c1
#define TH1520_CSR_MCOR       0x7c2
#define TH1520_CSR_MHINT      0x7c5
#define TH1520_CSR_CPUID      0xfc0
#define TH1520_CSR_MAPBADDR   0xfc1
#define TH1520_CSR_SXSTATUS   0x5c0

/* TH1520 AP clock/reset base addresses */
#define TH1520_AP_CLK_BASE    0xffef010000UL
#define TH1520_AP_RST_BASE    0xffef014000UL

#endif /* !_ASMLANGUAGE */

#endif /* ZEPHYR_SOC_THEAD_TH1520_SOC_H_ */
