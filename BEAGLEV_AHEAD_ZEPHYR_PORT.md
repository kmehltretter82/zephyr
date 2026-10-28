# BeagleV-Ahead (T-Head TH1520) — Zephyr RTOS Port & QEMU Emulation Guide

This document describes the complete **Zephyr RTOS** SoC (`th1520`) and board (`beaglev_ahead`) support package for the **BeagleV-Ahead** (powered by the quad-core 64-bit RISC-V **T-Head TH1520 / XuanTie C910** SoC), along with the peripheral drivers, upper-layer stacks, MikroBUS Click board models, and AMP/SRAM targets across **Tracks 1–4** verified against the **BeagleV-Ahead QEMU fork** (`qemu-th1520`).

---

## 1. Supported Board Targets (6 Configurations)

All 6 Zephyr execution modes are implemented and verified on `qemu-system-riscv64 -machine beaglev-ahead`:

| Board Target | Privilege Mode | Cores | Memory Origin | Boot Method | QEMU Flag |
|---|---|---|---|---|---|
| `beaglev_ahead/th1520` | M-mode | 1 (`hart0`) | `0x00200000` (DRAM) | Direct M-mode entry | `-bios <zephyr.elf>` |
| `beaglev_ahead/th1520/smp` | M-mode SMP | 4 (`hart0..3`) | `0x00200000` (DRAM) | Direct M-mode 4-core SMP | `-bios <zephyr.elf>` |
| `beaglev_ahead/th1520/smode` | M+S Internal SBI | 1 (`hart0`) | `0x00200000` (DRAM) | Zephyr internal M-mode SBI stub -> S-mode kernel | `-bios <zephyr.elf>` |
| `beaglev_ahead/th1520/opensbi` | S-mode (OpenSBI) | 1 (`hart0`) | `0x00200000` (DRAM + 2 MiB) | External OpenSBI -> S-mode | `-kernel <zephyr.elf>` |
| `beaglev_ahead/th1520/opensbi_smp` | S-mode SMP (OpenSBI) | 4 (`hart0..3`) | `0x00200000` (DRAM + 2 MiB) | External OpenSBI -> 4-core S-mode SMP (`SBI_EXT_HSM`) | `-kernel <zephyr.elf>` |
| `beaglev_ahead/th1520/sram` | M-mode (On-Chip SRAM) | 1 (`hart0` / AMP) | `0xffe0000000` (1.5 MiB SRAM) | Zero-DRAM execution out of On-Chip SRAM + AMP Shared-Memory IPC | `-bios <zephyr.elf>` |

---

## 2. Emulated & Verified Hardware Subsystems (18/18 Passing)

Every peripheral below is wired in the Zephyr devicetree (`dts/riscv/thead/th1520.dtsi` and `boards/beagle/beaglev_ahead/beaglev_ahead.dts`) and verified end-to-end against `qemu-th1520`:

| # | Track | Subsystem | Zephyr Compatible / Driver | MMIO Base | PLIC IRQ | BeagleV-Ahead Function |
|---|---|---|---|---|---|---|
| 1 | Base | **CPUs (`cpu0..cpu3`) & CLINT** | `thead,c910` + `thead,c900-clint` (`riscv_machine_timer`) | `0xffdc000000` | — | 4x 64-bit XuanTie C910 + T-Head CSRs (`mxstatus`, `mhcr`, `mhint`, `mcpuid`) + 3 MHz CLINT |
| 2 | Base | **PLIC & UART0..UART5** | `thead,c900-plic` (`intc_plic`) + `ns16550` (`uart_ns16550`) | `0xffd8000000` (PLIC)<br>`0xffe7014000` (`uart0`)<br>`0xffe7f04000` (`uart3`)<br>`0xfff7f08000` (`uart4`) | `36` (`uart0`)<br>`39` (`uart3`)<br>`40` (`uart4`) | `uart0`: Console<br>`uart3`: `mikrobus_serial`<br>`uart4`: AP6203BM Bluetooth H4 HCI |
| 3 | Base | **GPIO0..GPIO4, AOGPIO** | `snps,designware-gpio` (`gpio_dw`) | `0xffec005000`..`0xfff7f1c000` | `55`..`59`, `76` | `gpio4`: 5x User LEDs<br>`gpio2`: AP6203BM Wi-Fi/BT control lines<br>`gpio0`: MikroBUS INT |
| 4 | Base | **I2C0 & AT24C32 EEPROM** | `snps,designware-i2c` (`i2c_dw`) + `atmel,at24` | `0xffe7f20000` | `44` | Onboard 4 KiB AT24C32 EEPROM at `0x50` |
| 5 | Base | **AON_I2C & DA9063 PMIC** | `snps,designware-i2c` (`i2c_dw`) | `0xfffff4c000` | `79` | Onboard Dialog DA9063 PMIC at `0x58` (`VBCORE1_A`, `VBCORE2_A`, `DVC_2`) |
| 6 | Base | **SPI0 (`mikrobus_spi`)** | `snps,designware-spi` (`spi_dw`) | `0xffe700c000` | `54` | Full-duplex DesignWare SSI master |
| 7 | Base | **TIMER0..TIMER7** | `snps,dw-timers` (`counter_dw`) | `0xffefc32000` | `16`..`23` | 8x DesignWare 32-bit countdown timers with alarm IRQ callbacks |
| 8 | Base | **WDT0, WDT1** | `snps,designware-watchdog` (`wdt_dw`) | `0xffefc30000` | `24`, `25` | 2x DesignWare hardware watchdogs + `AP_RESET` disarm |
| 9 | **Track 1.1** | **TH1520 6-Channel PWM** | `thead,th1520-pwm` (`drivers/pwm/pwm_th1520.c`) | `0xffec01c000` | — | 6-channel PWM controller wired to `mikrobus_header` PWM pin |
| 10 | **Track 1.2** | **TH1520 Pinmux / Pinctrl** | `thead,th1520-pinctrl` (`drivers/pinctrl/pinctrl_th1520.c`) | `0xfffff4a000` (AOSYS)<br>`0xffec007000` (PADCTRL1)<br>`0xffe7f3c000` (PADCTRL0) | — | Dynamic pad multiplexing, pull-up/pull-down, Schmitt trigger & drive strength |
| 11 | **Track 1.3a** | **Moortec MR75203 PVT** | `moortec,mr75203` (`drivers/sensor/moortec/mr75203/mr75203.c`) | `0xfffff4e000` | — | On-die temperature (`SENSOR_CHAN_DIE_TEMP`) & core voltage (`SENSOR_CHAN_VOLTAGE`) |
| 12 | **Track 1.3b** | **APM X-Gene / DW RTC** | `apm,xgene-rtc` (`drivers/rtc/rtc_xgene.c`) | `0xfffff40000` | `74` | Hardware Real-Time Clock (`rtc_set_time` / `rtc_get_time`) |
| 13 | **Track 1.4** | **DesignWare AXI DMA** | `snps,axi-dma-1.01a` (`drivers/dma/dma_dw_axi.c`) | `0xffefc00000` | `27` | 8-channel 64-bit AXI DMAC (M2M buffer copy + M2P UART3 TX DMA) |
| 14 | **Track 2.1** | **Gigabit Ethernet (`gmac0`)** | `thead,th1520-gmac` (`drivers/ethernet/eth_th1520_gmac.c`) | `0xffe7070000` + APB `0xffec003000` | `66` | DesignWare GMAC + RTL8211F PHY + Zephyr IPv4/UDP socket stack |
| 15 | **Track 2.2** | **Bluetooth H4 HCI (`ap6203bm`)** | `zephyr,bt-hci-uart` (`CONFIG_BT_H4` over `uart4`) | `0xfff7f08000` | `40` | Onboard AP6203BM BLE H4 HCI controller, BLE advertising & active scanning |
| 16 | **Track 2.3** | **Synopsys DWC MSHC eMMC** | `thead,th1520-dwcmshc` (`drivers/disk/sdmmc_dwcmshc.c`) | `0xffe7080000` | `62` | SDHCI/eMMC ADMA2/PIO block storage (`disk_access` `"EMMC"` read/write) |
| 17 | **Track 3** | **MikroE Click Boards** | `bosch,bme280` (`i2c3` @ `0x76`)<br>`microchip,mcp251xfd` (`spi0` CS0, INT `gpio0` pin 20) | `0xffec014000` (`i2c3`)<br>`0xffe700c000` (`spi0`) | `47` (`i2c3`)<br>`54` (`spi0`), `56` (`gpio0`) | Bosch BME280 Weather Click (`T`, `P`, `RH`) + Microchip MCP2518FD CAN-FD Click (16-byte BRS CAN-FD TX/RX) |
| 18 | **Track 4** | **TH1520 Mailbox & SRAM AMP** | `thead,th1520-mbox` (`drivers/mbox/mbox_th1520.c`) | `0xffffc38000` (`mbox0`)<br>`0xffe0000000` (`sram0`) | `28` | Hardware Mailbox IPC + Shared-Memory AMP RPMsg ring + On-Chip SRAM execution target |

---

## 3. Building & Running the 18-Subsystem Verification Suite

```bash
export ZEPHYR_BASE=/Users/kmehltretter/codex/zephyr/zp/zephyr
export ZEPHYR_SDK_INSTALL_DIR=/Users/kmehltretter/codex/zephyr/sdk
export PATH="/Users/kmehltretter/codex/zephyr/.venv/bin:$PATH"
cd /Users/kmehltretter/codex/zephyr/zp

# 1. M-Mode Direct DRAM Boot (beaglev_ahead/th1520)
west build -b beaglev_ahead/th1520 -d build-verify-mmode zephyr/samples/boards/beagle/beaglev_ahead_verify
/Users/kmehltretter/codex/zephyr/qemu-build/qemu-system-riscv64 -machine beaglev-ahead -nographic \
  -bios build-verify-mmode/zephyr/zephyr.elf

# 2. S-Mode OpenSBI Boot (beaglev_ahead/th1520/opensbi)
west build -b beaglev_ahead/th1520/opensbi -d build-verify-opensbi zephyr/samples/boards/beagle/beaglev_ahead_verify
/Users/kmehltretter/codex/zephyr/qemu-build/qemu-system-riscv64 -machine beaglev-ahead -nographic \
  -kernel build-verify-opensbi/zephyr/zephyr.elf

# 3. Track 4: On-Chip SRAM Execution at 0xffe0000000 (beaglev_ahead/th1520/sram)
west build -b beaglev_ahead/th1520/sram -d build-verify-sram zephyr/samples/boards/beagle/beaglev_ahead_verify
/Users/kmehltretter/codex/zephyr/qemu-build/qemu-system-riscv64 -machine beaglev-ahead -nographic \
  -bios build-verify-sram/zephyr/zephyr.elf
```

---

## 4. Verification Output Log (18/18 Passing)

```text
*** Booting Zephyr OS build v4.5.0-rc3 ***

================================================================
 BeagleV-Ahead (T-Head TH1520) Full Hardware & Track 1-4 Suite
 Board: beaglev_ahead | SoC: Quad-Core XuanTie C910 (RV64GC)
================================================================

[1/18] CPU, Vendor CSRs & SMP Verification
  Mode: M-Mode (Direct) (mvendorid=0x5b7, marchid=0x0, mimpid=0x0)
  T-Head CSRs: MXSTATUS=0x00c08000 MHCR=0x000011ff MHINT=0x0016e30c MCPUID=0x0804010d
  Active CPUs: 1
  [PASS] XuanTie C910 CPU & vendor CSRs verified

[2/18] PLIC Interrupt-Driven UART3 (mikrobus_serial) Verification
  TX/RX via PLIC IRQ 39: 'Z' (0x5a)
  [PASS] UART3 PLIC interrupt-driven TX/RX verified

[3/18] DesignWare GPIO, Onboard User LEDs & AP6203BM Control Verification
  Toggled 5 onboard User LEDs (GPIO4_8..12) and AP6203BM WL_REG_ON/BT_REG_ON (GPIO2_31/28)
  [PASS] DesignWare GPIO & onboard lines verified

[4/18] DesignWare I2C0 & Onboard AT24C32 EEPROM (0x50) Verification
  EEPROM magic readback @ 0x0000: "BVA-TH1520-OK!"
  [PASS] DesignWare I2C0 & AT24C32 EEPROM verified

[5/18] AON I2C & Dialog DA9063 PMIC (0x58) Verification
  DA9063 VBCORE1_A=0x7a, VBCORE2_A=0x78, DVC_2=0x81 (masked 0x81)
  [PASS] AON I2C & Dialog DA9063 PMIC verified

[6/18] DesignWare SPI0 (mikrobus_spi) Verification
  Transceived 8 bytes on spi0 @ 10 MHz via PLIC IRQ 54
  [PASS] DesignWare SPI0 (mikrobus_spi) verified

[7/18] DesignWare APB Timer0 (PLIC IRQ 16 @ 125 MHz) Verification
  Timer0 alarm fired via PLIC IRQ 16 (configured 625000 ticks)
  [PASS] DesignWare APB Timer0 verified

[8/18] DesignWare Watchdog0 (wdt0 @ 125 MHz, PLIC IRQ 24) Verification
  Watchdog0 armed, fed 3x, fired PLIC IRQ 24 & disarmed via AP_RESET
  [PASS] DesignWare Watchdog0 verified

[9/18] [Track 1.1] TH1520 6-Channel PWM (pwm0 @ 0xffec01c000) Verification
  Ch0 (20 kHz, 25%): CTRL=0x120 PERIOD=5000 FP=1250
  Ch1 (10 kHz, 75% inv): CTRL=0x020 PERIOD=10000 FP=7500
  [PASS] TH1520 6-Channel PWM verified

[10/18] [Track 1.2] TH1520 Pinctrl (PADCTRL0/1_APSYS & AOSYS) Verification
  PADCTRL0_APSYS PADCFG[5]=0x02280208, MUXCFG[0]=0x00001000, MUXCFG[1]=0x00011000
  [PASS] TH1520 Pinctrl pad multiplexing, pull-ups & drive-strength verified

[11/18] [Track 1.3a] Moortec MR75203 On-Die PVT Sensor Verification
  On-Die Temperature: 45.286 C, Core Voltage: 0.800 V
  [PASS] Moortec MR75203 On-Die PVT temperature & voltage verified

[12/18] [Track 1.3b] DesignWare / X-Gene RTC (rtc0 @ 0xfffff40000) Verification
  RTC UTC readback: 2026-10-25 14:30:45
  [PASS] DesignWare / X-Gene RTC calendar read/write verified

[13/18] [Track 1.4] DesignWare AXI DMA (dmac0 @ 0xffefc00000, IRQ 27) Verification
  AXI DMA Ch0 M2M (64 B) & Ch1 M2P -> UART3 THR (8 chars) completed via IRQ 27
  [PASS] DesignWare AXI DMA M2M & M2P transfers verified

[14/18] [Track 2.1] Gigabit Ethernet (gmac0 @ 0xffe7070000 + IPv4/UDP) Verification
  GMAC0 MAC=02:be:a6:15:20:01 IPv4=192.0.2.10 L2 TX/RX & UDP/4242 "ZEPHYR_TH1520_GMAC0_UDP_OK" OK
  [PASS] Gigabit Ethernet (gmac0 + RTL8211F PHY + Zephyr IPv4/UDP) verified

[15/18] [Track 2.2] Bluetooth H4 UART4 HCI (AP6203BM BLE Adv/Scan) Verification
  AP6203BM H4 HCI initialized, BLE advertising & active scan (1 adv seen) OK
  [PASS] Bluetooth H4 UART4 HCI (AP6203BM) verified

[16/18] [Track 2.3] Synopsys DWC MSHC eMMC0 (mshc0 @ 0xffe7080000) Verification
  eMMC capacity: 4096 sectors (2048 KiB), LBA 16..17 write/read (1024 B) verified
  [PASS] Synopsys DWC MSHC eMMC0 block storage verified

[17/18] [Track 3] MikroE Click Boards (BME280 Weather + MCP2518FD CAN-FD)
  BME280 Weather Click (i2c3 @ 0x76): T=25.10 C, P=100.65 kPa, RH=53.36 %
  MCP2518FD CAN-FD Click (spi0): 16-byte BRS CAN-FD frame ID=0x123 TX->RX OK
  [PASS] MikroE Weather Click (BME280) & CAN-FD Click (MCP2518FD) verified

[18/18] [Track 4] TH1520 Hardware Mailbox (mbox0) & On-Chip SRAM AMP IPC
  Executing PC=0xffe00005fc (On-Chip SRAM 0xffe0000000), MBOX0 IRQ 28 + SRAM IPC "AMP_RPMSG_CORE0_TO_CORE3_SRAM_OK" OK
  [PASS] TH1520 Hardware Mailbox & AMP Shared-SRAM IPC verified

================================================================
 ALL 18/18 BEAGLEV-AHEAD HARDWARE & TRACK 1-4 SUBSYSTEMS PASSED!
================================================================
```
