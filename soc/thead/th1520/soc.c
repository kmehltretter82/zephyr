/*
 * Copyright (c) 2026 BeagleV-Ahead Zephyr Port
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/poweroff.h>
#include <soc.h>

#ifdef CONFIG_RISCV_S_MODE
#include <zephyr/arch/riscv/sbi.h>

void sys_arch_reboot(int type)
{
	ARG_UNUSED(type);

	register unsigned long a0 __asm__("a0") = SBI_SRST_RESET_TYPE_COLD_REBOOT;
	register unsigned long a1 __asm__("a1") = SBI_SRST_RESET_REASON_NONE;
	register unsigned long a6 __asm__("a6") = SBI_FUNC_SYSTEM_RESET;
	register unsigned long a7 __asm__("a7") = SBI_EXT_SRST;

	__asm__ volatile("ecall"
			 : "+r"(a0), "+r"(a1)
			 : "r"(a6), "r"(a7)
			 : "memory");
}

#ifdef CONFIG_POWEROFF
void z_sys_poweroff(void)
{
	register unsigned long a0 __asm__("a0") = SBI_SRST_RESET_TYPE_SHUTDOWN;
	register unsigned long a1 __asm__("a1") = SBI_SRST_RESET_REASON_NONE;
	register unsigned long a6 __asm__("a6") = SBI_FUNC_SYSTEM_RESET;
	register unsigned long a7 __asm__("a7") = SBI_EXT_SRST;

	__asm__ volatile("ecall"
			 : "+r"(a0), "+r"(a1)
			 : "r"(a6), "r"(a7)
			 : "memory");
	CODE_UNREACHABLE;
}
#endif /* CONFIG_POWEROFF */
#endif /* CONFIG_RISCV_S_MODE */
