# SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: Apache-2.0
#
# Boot Linux on cores 0-2 and Zephyr on core 3 of the Raspberry Pi 4.
#
# Zephyr is loaded into memory hidden from Linux and its entry point is
# written to core 3's spin-table release address. Core 3 leaves the
# firmware's wfe loop on the first sev Linux issues while releasing
# cpu1, after Linux has set up the GIC distributor. nr_cpus=3 keeps
# Linux away from core 3's release address.
#
# Zephyr sits above the CMA alloc-range (0x0-0x30000000) of the
# firmware's devicetree and below U-Boot.
#
# Optional variables:
#   amp_linux_start=1 leave core 3 parked; the rpi4_amp module starts it
#   amp_tftp=1        fetch zephyr-amp.bin over TFTP instead of the SD card
#   amp_extra_args    appended to the kernel command line

setenv amp_base 0x30000000
setenv amp_release 0xf0
setenv amp_fdt 0x0f000000

mw.b 0x30800000 0 0x10000
if test -z "${amp_linux_start}"; then
	if test -n "${amp_tftp}"; then
		tftpboot ${amp_base} zephyr-amp.bin
	else
		load mmc 0:1 ${amp_base} zephyr-amp.bin
	fi
	mw.q ${amp_release} ${amp_base}
fi

# ${fdt_addr} is also U-Boot's live control devicetree: edit a copy
fdt addr ${fdt_addr}
fdt move ${fdt_addr} ${amp_fdt} 0x20000
fdt mknode /reserved-memory zephyr@30000000
fdt set /reserved-memory/zephyr@30000000 reg <0x0 0x30000000 0x00910000>
fdt set /reserved-memory/zephyr@30000000 no-map
fdt mknode /reserved-memory vdev0buffer@30910000
fdt set /reserved-memory/vdev0buffer@30910000 compatible "shared-dma-pool"
fdt set /reserved-memory/vdev0buffer@30910000 reg <0x0 0x30910000 0x000f0000>
fdt set /reserved-memory/vdev0buffer@30910000 no-map
fdt set /reserved-memory/vdev0buffer@30910000 phandle <0x7a6d0001>

# For the rpi4_amp_rproc remoteproc driver: rpmsg buffer pool and the SPI
# the remote CPU sets pending for Linux
fdt mknode / zephyr-amp
fdt set /zephyr-amp compatible "rpi4-amp,zephyr"
fdt set /zephyr-amp memory-region <0x7a6d0001>
fdt set /zephyr-amp interrupts <0 221 1>
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
