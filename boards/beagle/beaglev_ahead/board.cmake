# SPDX-License-Identifier: Apache-2.0

set(SUPPORTED_EMU_PLATFORMS qemu)
set(QEMU_BINARY_SUFFIX riscv64)
set(QEMU_CPU_TYPE "thead-c910")

if(NOT DEFINED ENV{QEMU_BIN_PATH} AND EXISTS "${ZEPHYR_BASE}/../../qemu-build/qemu-system-riscv64")
  set(ENV{QEMU_BIN_PATH} "${ZEPHYR_BASE}/../../qemu-build")
endif()
list(APPEND QEMU_BINARY_HINTS "${ZEPHYR_BASE}/../../qemu-build")

if(CONFIG_RISCV_S_MODE_EXTERNAL_SBI)
  set(qemu_bios default)
else()
  set(qemu_bios none)
endif()

set(QEMU_BOARD_FLAGS
  -machine beaglev-ahead
  -bios ${qemu_bios}
  -m 4G
  )

include(${ZEPHYR_BASE}/boards/common/qemu.board.cmake)
