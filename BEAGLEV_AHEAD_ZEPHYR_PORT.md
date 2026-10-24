# BeagleV-Ahead (T-Head TH1520) — Zephyr RTOS Port & QEMU Emulation Guide

This document describes the complete **Zephyr RTOS** SoC (`th1520`) and board (`beaglev_ahead`) support package for the **BeagleV-Ahead** (powered by the quad-core 64-bit RISC-V **T-Head TH1520 / XuanTie C910** SoC), along with the fixes and enhancements made to the **BeagleV-Ahead QEMU fork** (`qemu-th1520`) and Zephyr drivers.

---

## 1. Supported Board Targets (5 Configurations)

All 5 Zephyr execution modes are implemented and verified on `qemu-system-riscv64 -machine beaglev-ahead`:

| Board Target | Privilege Mode | Cores | Memory Origin | Boot Method | QEMU `-bios` Flag |
|---|---|---|---|---|---|
| `beaglev_ahead/th1520` | M-mode | 1 (`hart0`) | `0x00000000` (4 GiB DRAM) | Direct M-mode entry | `-bios none` |
| `beaglev_ahead/th1520/smp` | M-mode SMP | 4 (`hart0..3`) | `0x00000000` (4 GiB DRAM) | Direct M-mode 4-core SMP | `-bios none` |
| `beaglev_ahead/th1520/smode` | M+S Internal SBI | 1 (`hart0`) | `0x00000000` (4 GiB DRAM) | Zephyr internal M-mode SBI stub -> S-mode kernel | `-bios none` |
| `beaglev_ahead/th1520/opensbi` | S-mode (OpenSBI) | 1 (`hart0`) | `0x00200000` (DRAM + 2 MiB) | External OpenSBI (`fw_dynamic.bin`) -> S-mode | `-bios default` |
| `beaglev_ahead/th1520/opensbi_smp` | S-mode SMP (OpenSBI) | 4 (`hart0..3`) | `0x00200000` (DRAM + 2 MiB) | External OpenSBI (`fw_dynamic.bin`) -> 4-core S-mode SMP (`SBI_EXT_HSM`) | `-bios default` |

---

## 2. Emulated & Verified Hardware Subsystems

Every peripheral below is wired in the Zephyr devicetree (`dts/riscv/thead/th1520.dtsi` and `boards/beagle/beaglev_ahead/beaglev_ahead.dts`) and verified end-to-end against `qemu-th1520`:

| Subsystem | Zephyr Compatible / Driver | MMIO Base | PLIC IRQ | BeagleV-Ahead Function |
|---|---|---|---|---|
| **CPUs (`cpu0..cpu3`)** | `thead,c910`, `riscv` | — | — | 4x 64-bit XuanTie C910 (`rv64imafdc_zicsr_zifencei`) + T-Head CSRs (`CSR_TH_MXSTATUS`, `CSR_TH_SXSTATUS`, `CSR_TH_CPUID`) |
| **CLINT** | `thead,c900-clint`, `riscv,clint0` (`riscv_machine_timer`) | `0xffdc000000` | — | 3 MHz system timer (`csr_read(time)` + 32-bit split `mtimecmp`) & M-mode IPI (`msip`) |
| **PLIC** | `thead,c900-plic`, `sifive,plic-1.0.0` (`intc_plic`) | `0xffd8000000` | — | 240 interrupt sources, 8 hart contexts (M/S for harts 0..3), `PLIC_CTRL` (`0xffd81ffffc`) |
| **UART0..UART5** | `snps,dw-apb-uart` (`uart_ns16550`) | `0xffe7014000` (`uart0`)<br>`0xffe7f0c000` (`uart3`) | `36` (`uart0`)<br>`39` (`uart3`) | `uart0`: Debug console (`115200 8N1`, interrupt-driven)<br>`uart3`: `mikrobus_serial` |
| **GPIO0..GPIO4, AOGPIO** | `snps,dw-apb-gpio` (`gpio_dw`) | `0xffec005000`..`0xfff7f1c000` | `52`..`55`, `158` | `gpio4` pins `8..12`: Onboard 5x User LEDs (`led0..led4` / `USR0..USR4`)<br>`gpio2`: AP6203BM Wi-Fi/BT control lines<br>`mikrobus_header`: MikroBUS GPIO nexus |
| **I2C0..I2C5, AON_I2C** | `snps,designware-i2c` (`i2c_dw`) | `0xffe7f20000` (`i2c0`)<br>`0xffe7f28000` (`i2c3`)<br>`0xfffff4c000` (`aon_i2c`) | `42` (`i2c0`)<br>`45` (`i2c3`)<br>`160` (`aon_i2c`) | `i2c0`: Onboard 4 KiB AT24C32 EEPROM at `0x50` (`atmel,at24`)<br>`i2c3`: `mikrobus_i2c`<br>`aon_i2c`: Onboard Dialog DA9063 PMIC at `0x5a` |
| **SPI0** | `snps,designware-spi` (`spi_dw`) | `0xffe700c000` | `50` | `mikrobus_spi` (full-duplex SSI master with CS GPIO `gpio2` pin 15) |
| **TIMER0..TIMER7** | `snps,dw-timers` (`counter_dw`) | `0xffefc32000` (`timer0`..`3`)<br>`0xfff7f20000` (`timer4`..`7`) | `64`..`71` | 8x DesignWare 32-bit countdown timers with alarm IRQ callbacks |
| **WDT0, WDT1** | `snps,designware-watchdog` (`wdt_dw`) | `0xffefc30000` (`wdt0`)<br>`0xffefc31000` (`wdt1`) | `72`, `73` | 2x DesignWare hardware watchdogs (`TOP`/`TOP_INIT`, `CRR` kick `0x76`, `EOI`) |

---

## 3. Files Created & Modified

### 3.1 New Zephyr SoC & Board Files (`/Users/kmehltretter/codex/zephyr/zp/zephyr`)

- **Devicetree Bindings & SoC DTSI:**
  - `dts/bindings/vendor-prefixes.txt` — Registered `thead` vendor prefix (`T-Head Semiconductor Co., Ltd.`).
  - `dts/bindings/cpu/thead,c910.yaml` — CPU binding for XuanTie C910 cores.
  - `dts/riscv/thead/th1520.dtsi` — Complete TH1520 SoC devicetree (`cpus`, `clint`, `plic`, `uart0..5`, `i2c0..5`, `aon_i2c`, `spi0`, `gpio0..4`, `aogpio`, `timer0..7`, `wdt0..1`).
- **T-Head TH1520 SoC Layer (`soc/thead/th1520/`):**
  - `soc.yml`, `Kconfig.soc`, `Kconfig`, `Kconfig.defconfig`, `CMakeLists.txt`
  - `soc.h` — Defines T-Head XuanTie C910 vendor CSRs (`CSR_TH_MXSTATUS`, `CSR_TH_SXSTATUS`, `CSR_TH_MHCR`, `CSR_TH_MHINT`, `CSR_TH_MCOR`, `CSR_TH_CPUID`, `CSR_TH_MAPBADDR`) and `TH1520_PLIC_CTRL_ADDR` (`0xffd81ffffc`).
  - `soc.c` — Early `soc_early_init_hook()` delegating PLIC S-mode access (`PLIC_CTRL = 1`) when running in M-mode.
- **BeagleV-Ahead Board Layer (`boards/beagle/beaglev_ahead/`):**
  - `board.yml`, `board.cmake`, `Kconfig.beaglev_ahead`, `Kconfig.defconfig`
  - `beaglev_ahead.dts` — Base board DTS (`4 GiB` DRAM, `gpio-leds` `USR0..USR4`, `eeprom0: eeprom@50` on `i2c0`, `mikrobus_header`, aliases).
  - `beaglev_ahead_th1520_defconfig` — M-mode single-core defconfig.
  - `beaglev_ahead_th1520_smp.dts` & `beaglev_ahead_th1520_smp_defconfig` — M-mode 4-core SMP.
  - `beaglev_ahead_th1520_smode.dts` & `beaglev_ahead_th1520_smode_defconfig` — S-mode with Zephyr internal M-mode SBI.
  - `beaglev_ahead_th1520_opensbi.dts` & `beaglev_ahead_th1520_opensbi_defconfig` — S-mode with external OpenSBI (`0x00200000` load offset).
  - `beaglev_ahead_th1520_opensbi_smp.dts` & `beaglev_ahead_th1520_opensbi_smp_defconfig` — S-mode 4-core SMP with external OpenSBI.

### 3.2 Zephyr Kernel & Driver Fixes (`/Users/kmehltretter/codex/zephyr/zp/zephyr`)

1. **`drivers/timer/riscv_machine_timer.c` (`thead,c900-clint` Support):**
   - `thead,c900-clint` is a 32-bit APB peripheral without an MMIO `mtime` register; C910 reads the 64-bit counter via `csr_read(time)` and requires two 32-bit `sw` writes (`0xffffffff` high-word guard followed by low word, then high word) to `mtimecmp`.
2. **`arch/riscv/core/sbi.S` & `arch/riscv/core/reset.S` (Zephyr Internal SBI on TH1520):**
   - Updated `sbi_ecall_handler` in `sbi.S` to perform two 32-bit `sw` writes to `MTIMECMP0` when `!DT_REG_HAS_NAME(DT_INST(0, riscv_machine_timer), mtime)`.
   - Guarded `csrw pmpaddr0` / `csrw pmpcfg0` in `reset.S` with `#if CONFIG_PMP_SLOTS > 0` and set `thead,c900-plic` `PLIC_CTRL = 1` (`0xffd81ffffc`) before `mret` into S-mode.
3. **`drivers/interrupt_controller/intc_plic.c` (S-mode PLIC Context & IRQ Routing):**
   - Added `PLIC_IRQN(n)` so S-mode kernels connect `RISCV_IRQ_SEXT` (IRQ 9) instead of M-mode `11` (`MEIP`) and map `hart_context` to S-mode PLIC contexts `{1, 3, 5, 7}` while keeping `DT_INST_IRQN(n)` (`11`) in `IRQ_PARENT_ENTRY_DEFINE`.
4. **`drivers/i2c/i2c_dw.c` (DesignWare I2C NACK / `TX_ABRT` Error Handling):**
   - Fixed `i2c_dw_data_send()` and `i2c_dw_isr()` so an I2C NACK (`TX_ABRT`) sets `I2C_DW_CMD_ERROR` and clears `tx_abrt`, ensuring `i2c_transfer()` returns `-EIO` on missing addresses and `i2c scan` detects only present slaves.
5. **`drivers/watchdog/wdt_dw.{c,h}` & `wdt_dw_common.{c,h}` (64-bit MMIO & Counter Width):**
   - Changed `uint32_t base` to `uintptr_t base` across all functions so 64-bit MMIO addresses above 4 GiB (`0xffefc30000`) are not truncated.
   - Defaulted `dw_wdt_cnt_width_get()` to 32 bits when `WDT_COMP_PARAM_1.WDT_CNT_WIDTH` is `0`, and programmed both `TOP_INIT` and `TOP` in `dw_wdt_configure()`.

### 3.3 QEMU Fork Fixes (`/Users/kmehltretter/codex/zephyr/qemu-th1520`)

1. **`hw/riscv/th1520.c` (`beaglev_ahead_machine_done` Direct-Boot Entry):**
   - When `-bios none -kernel <elf>` is passed, `riscv_load_kernel()` loads the ELF and returns its entry point (`kernel_entry`), e.g. `0x00000000` (DRAM) or `0xffe0000000` (SRAM). Updated `beaglev_ahead_machine_done()` to pass `kernel_entry` to `riscv_setup_rom_reset_vec()` instead of hardcoded `ram_base`.
2. **`hw/riscv/boot.c` (`riscv_compute_fdt_addr` High-Address SRAM Support):**
   - When a kernel is linked in high MMIO SRAM (`0xffe0000000`, above DRAM), `riscv_compute_fdt_addr()` previously clamped `temp` to `kernel_start` and triggered `g_assert(dtb_start < dtb_start_limit)`. Guarded the `MIN(dram_end, kernel_start)` clamp to apply only when `kernel_start > dram_base && kernel_start < dram_end`.
3. **`hw/i2c/designware_i2c.c` (DesignWare I2C `RFNE`/`RFF` Status & Repeated-Start Ordering):**
   - Added `dw_i2c_update_rx_status()` to set/clear `R_DW_IC_STATUS_RFNE_MASK` (bit 3) and `R_DW_IC_STATUS_RFF_MASK` (bit 4) in `DW_IC_STATUS` whenever `rx_fifo` is pushed, popped, or reset.
   - Moved `R_DW_IC_DATA_CMD_RESTART_MASK` handling before `/* Send data */` so combined write+read (`i2c_write_read` / EEPROM random reads) cleanly transition the slave to read direction on repeated START.
   - All 8 `/riscv64/beaglev-ahead/dw-i2c/*` and all 7 `/riscv64/beaglev-ahead/boot/*` QEMU QTests pass 100%.

---

## 4. Building & Running Zephyr on BeagleV-Ahead QEMU

### 4.1 Environment Setup

```bash
export ZEPHYR_BASE=/Users/kmehltretter/codex/zephyr/zp/zephyr
export ZEPHYR_SDK_INSTALL_DIR=/Users/kmehltretter/codex/zephyr/sdk
export PATH="/Users/kmehltretter/codex/zephyr/.venv/bin:$PATH"
```

### 4.2 Running the 8-Subsystem Hardware Verification Suite (`beaglev_ahead_verify`)

The standalone verification application in `/Users/kmehltretter/codex/zephyr/zp/beaglev_ahead_verify` tests all 8 hardware subsystems:
1. XuanTie C910 CPU CSRs (`CSR_TH_MXSTATUS`, `CSR_TH_SXSTATUS`, `CSR_TH_CPUID`) & SMP Hart Discovery
2. 3 MHz System Timer (`thead,c900-clint`) & Preemptive Multi-Threading
3. DesignWare GPIO (`gpio4` onboard LEDs `USR0..USR4`, `gpio2` Wi-Fi/BT lines, `aogpio`, and GPIO loopback interrupt)
4. DesignWare I2C0 + Onboard AT24C32 4 KiB EEPROM (`0x50`) write/read/restore verification
5. DesignWare AON_I2C + Onboard Dialog DA9063 PMIC (`0x5a`) Silicon/Variant ID & Buck converter telemetry
6. DesignWare SPI0 (`mikrobus_spi`) Master Transaction + MikroBUS I2C3 (`mikrobus_i2c`) & UART3 (`mikrobus_serial`) readiness
7. DesignWare APB Hardware Counter/Timer (`timer0` @ `0xffefc32000`) live countdown + PLIC IRQ 64 alarm callback
8. DesignWare Hardware Watchdog (`wdt0` @ `0xffefc30000`) timeout installation, setup, and `wdt_feed()` kick

```bash
cd /Users/kmehltretter/codex/zephyr/zp

# 1. M-mode single-core
west build -p always -b beaglev_ahead/th1520 -d build-verify-mmode beaglev_ahead_verify
west build -d build-verify-mmode -t run

# 2. M-mode 4-core SMP
west build -p always -b beaglev_ahead/th1520/smp -d build-verify-smp beaglev_ahead_verify
west build -d build-verify-smp -t run

# 3. S-mode with Zephyr internal SBI
west build -p always -b beaglev_ahead/th1520/smode -d build-verify-smode beaglev_ahead_verify
west build -d build-verify-smode -t run

# 4. S-mode with external OpenSBI
west build -p always -b beaglev_ahead/th1520/opensbi -d build-verify-opensbi beaglev_ahead_verify
west build -d build-verify-opensbi -t run

# 5. S-mode 4-core SMP with external OpenSBI
west build -p always -b beaglev_ahead/th1520/opensbi_smp -d build-verify-opensbi-smp beaglev_ahead_verify
west build -d build-verify-opensbi-smp -t run
```

### 4.3 Running the Interactive Zephyr Shell (`shell_module`) with GPIO, I2C & EEPROM Commands

```bash
cd /Users/kmehltretter/codex/zephyr/zp
west build -p always -b beaglev_ahead/th1520/smp -d build-shell zephyr/samples/subsys/shell/shell_module \
  -- -DCONFIG_GPIO=y -DCONFIG_GPIO_SHELL=y -DCONFIG_I2C=y -DCONFIG_I2C_SHELL=y -DCONFIG_EEPROM=y -DCONFIG_EEPROM_SHELL=y
west build -d build-shell -t run
```

Inside the interactive shell (`uart:~$`):
- `kernel cpus` — View all 4 active C910 cores
- `device list` — View all initialized TH1520 peripherals (`plic`, `gpio0..4`, `aogpio`, `uart0`, `uart3`, `i2c0`, `i2c3`, `aon_i2c`, `eeprom0`)
- `i2c scan i2c@ffe7f20000` — Scan `i2c0` (discovers `0x50` AT24C32 EEPROM)
- `i2c scan i2c@fffff4c000` — Scan `aon_i2c` (discovers `0x5a` DA9063 PMIC)
- `i2c read_byte i2c@fffff4c000 5a 81` — Read DA9063 PMIC `CHIP_ID` (`0x61`)
- `eeprom read eeprom@50 0x0 32` — Hexdump onboard EEPROM contents
- `gpio conf gpio@fff7f1c000 8 o1` — Toggle BeagleV-Ahead onboard LED `USR0`

---

## 5. Verification Output Logs

### 5.1 M-Mode 4-Core SMP (`beaglev_ahead/th1520/smp`)

```text
*** Booting Zephyr OS build v4.4.0-rc2-94-g1ff8dfca2c77 ***

====================================================================
  BeagleV-Ahead (T-Head TH1520) Zephyr RTOS Hardware Verification
  Board Target : beaglev_ahead/th1520/smp
  Execution    : M-mode (Direct Boot)
  Configured   : 4 CPU core(s)
====================================================================

[1/8] CPU & XuanTie C910 Vendor CSR Verification
  mvendorid    = 0x000005b7 (T-Head = 0x5b7)
  marchid      = 0x00000000
  mimpid       = 0x00000000
  th.mxstatus  = 0x00c08000 (C910 extended ISA enabled: bit22=1)
  th.cpuid     = 0x00000000
  SMP CPU[0] responded from hardware hart_id = 0
  SMP CPU[1] responded from hardware hart_id = 1
  SMP CPU[2] responded from hardware hart_id = 2
  SMP CPU[3] responded from hardware hart_id = 3
  [PASS] 1. CPU & SMP Verification (4/4 cores active)

[2/8] System Timer (thead,c900-clint @ 3 MHz) & Multi-Threading
  50 ms sleep : elapsed = 51 ms, cycles = 152834 (expected ~150000 @ 3 MHz)
  [PASS] 2. CLINT System Timer & Multi-Threading Verification

[3/8] DesignWare GPIO (5x User LEDs, AP6203BM Wi-Fi/BT, AOGPIO, IRQ)
  Toggled 5 BeagleV-Ahead User LEDs (USR0..USR4 on gpio4 pins 8..12)
  AP6203BM Wi-Fi/BT control lines on gpio2 pins 22/24/25 verified
  GPIO interrupt fired on gpio0 pin 0 (count = 1)
  [PASS] 3. DesignWare GPIO & Interrupt Verification

[4/8] DesignWare I2C0 & Onboard AT24C32 4 KiB EEPROM (0x50)
  EEPROM size  = 4096 bytes
  Wrote & verified 10 bytes at EEPROM offset 0x0040: "ZEPHYR-BVA"
  [PASS] 4. DesignWare I2C0 & AT24C32 EEPROM Verification

[5/8] DesignWare AON_I2C & Dialog DA9063 PMIC (0x5a)
  DA9063 CHIP_ID    = 0x61 (expected 0x61)
  DA9063 VARIANT_ID = 0x50
  DA9063 STATUS_A   = 0x01
  DA9063 BPRO_CONT  = 0x01 -> 0x03 -> 0x01
  [PASS] 5. DesignWare AON_I2C & DA9063 PMIC Verification

[6/8] MikroBUS Expansion Buses (SPI0, I2C3, UART3)
  SPI0 master transfer (4 bytes @ 1 MHz) completed cleanly
  [PASS] 6. MikroBUS SPI0, I2C3, and UART3 Verification

[7/8] DesignWare APB Hardware Timer (timer0 @ 0xffefc32000)
  timer0 counter freq = 100000000 Hz, top_value = 0xffffffff
  timer0 live countdown verified: 4294957296 -> 4294757296 (delta = 200000 ticks)
  timer0 alarm IRQ fired (ticks = 4294967295)
  [PASS] 7. DesignWare APB Timer0 & Alarm IRQ Verification

[8/8] DesignWare Hardware Watchdog (wdt0 @ 0xffefc30000)
  wdt0 configured (channel = 0, window = 2000 ms) and fed 3x via WDT_CRR (0x76)
  [PASS] 8. DesignWare Watchdog (wdt0) Verification

====================================================================
  SUMMARY: 8 PASSED, 0 FAILED (out of 8 subsystems)
  ALL 8/8 BEAGLEV-AHEAD HARDWARE SUBSYSTEMS PASSED!
====================================================================
```

### 5.2 OpenSBI S-Mode 4-Core SMP (`beaglev_ahead/th1520/opensbi_smp`)

```text
OpenSBI v1.7
   ____                    _____ ____ _____
  / __ \                  / ____|  _ \_   _|
 | |  | |_ __   ___ _ __ | (___ | |_) || |
 | |  | | '_ \ / _ \ '_ \ \___ \|  _ < | |
 | |__| | |_) |  __/ | | |____) | |_) || |_
  \____/| .__/ \___|_| |_|_____/|____/_____|
        | |
        |_|

Platform Name             : BeagleV-Ahead (T-Head TH1520)
Platform Features         : medeleg
Platform HART Count       : 4
Platform IPI Device       : aclint-mswi
Platform Timer Device     : aclint-mtimer @ 3000000Hz
Platform Console Device   : uart8250
...
*** Booting Zephyr OS build v4.4.0-rc2-94-g1ff8dfca2c77 ***

====================================================================
  BeagleV-Ahead (T-Head TH1520) Zephyr RTOS Hardware Verification
  Board Target : beaglev_ahead/th1520/opensbi_smp
  Execution    : S-mode (External OpenSBI)
  Configured   : 4 CPU core(s)
====================================================================
...
  SUMMARY: 8 PASSED, 0 FAILED (out of 8 subsystems)
  ALL 8/8 BEAGLEV-AHEAD HARDWARE SUBSYSTEMS PASSED!
====================================================================
```
