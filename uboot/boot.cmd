# SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: Apache-2.0
#
# Boot Linux on cores 0-2 of the Raspberry Pi 4 and leave core 3 parked in
# the firmware's spin-table loop for the cpu_rproc remoteproc driver.
# nr_cpus=3 keeps Linux away from core 3's release address.
#
# The memory for core 3 sits above the CMA alloc-range (0x0-0x30000000) of
# the firmware's devicetree and below U-Boot.
#
# amp_extra_args is appended to the kernel command line.

setenv amp_fdt 0x0f000000

# ${fdt_addr} is also U-Boot's live control devicetree: edit a copy
fdt addr ${fdt_addr}
fdt move ${fdt_addr} ${amp_fdt} 0x20000
fdt mknode /reserved-memory image@30000000
fdt set /reserved-memory/image@30000000 reg <0x0 0x30000000 0x00900000>
fdt set /reserved-memory/image@30000000 no-map
fdt set /reserved-memory/image@30000000 phandle <0x7a6d0002>
fdt mknode /reserved-memory vdev0vring0@30900000
fdt set /reserved-memory/vdev0vring0@30900000 reg <0x0 0x30900000 0x00008000>
fdt set /reserved-memory/vdev0vring0@30900000 no-map
fdt set /reserved-memory/vdev0vring0@30900000 phandle <0x7a6d0003>
fdt mknode /reserved-memory vdev0vring1@30908000
fdt set /reserved-memory/vdev0vring1@30908000 reg <0x0 0x30908000 0x00008000>
fdt set /reserved-memory/vdev0vring1@30908000 no-map
fdt set /reserved-memory/vdev0vring1@30908000 phandle <0x7a6d0004>
fdt mknode /reserved-memory vdev0buffer@30910000
fdt set /reserved-memory/vdev0buffer@30910000 compatible "shared-dma-pool"
fdt set /reserved-memory/vdev0buffer@30910000 reg <0x0 0x30910000 0x000f0000>
fdt set /reserved-memory/vdev0buffer@30910000 no-map
fdt set /reserved-memory/vdev0buffer@30910000 phandle <0x7a6d0001>

# For the cpu_rproc remoteproc driver: core 3, its memory, the SPI it sets
# pending for Linux and the SPI Linux sets pending for it
fdt get value amp_cpu /cpus/cpu@3 phandle
fdt mknode / zephyr-amp
fdt set /zephyr-amp compatible "amp,cpu-rproc"
fdt set /zephyr-amp cpus <${amp_cpu}>
fdt set /zephyr-amp memory-region <0x7a6d0002 0x7a6d0003 0x7a6d0004 0x7a6d0001>
fdt set /zephyr-amp interrupts <0 221 1 0 220 1>
fdt set /zephyr-amp firmware-name "zephyr-amp.elf"
fdt get value amp_args /chosen bootargs
setenv bootargs "${amp_args} nr_cpus=3 ${amp_extra_args}"

# Keep kernel, initrd and devicetree below 0x10000000 so the 512 MiB
# CMA pool still fits in its alloc-range
setenv initrd_high 0xffffffffffffffff
setenv fdt_high 0xffffffffffffffff

load mmc 0:1 0x08000000 kernel8.img
unzip 0x08000000 0x00200000
load mmc 0:1 0x0a000000 initramfs8
booti 0x00200000 0x0a000000:${filesize} ${amp_fdt}
