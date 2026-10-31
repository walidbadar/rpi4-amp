<!--
SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
SPDX-License-Identifier: Apache-2.0
-->

# Linux and Zephyr AMP on the Raspberry Pi 4

Linux runs on cores 0-2, Zephyr on core 3. Linux starts Zephyr with remoteproc, and the two talk
rpmsg over virtio. No hypervisor: both share the GIC-400 and the memory system, and each owns its
own peripherals.

## Memory map

| Range                       | Owner                                   |
|-----------------------------|-----------------------------------------|
| `0x00200000` - `0x0fffffff` | Linux kernel, initrd and devicetree     |
| `0x10000000` - `0x2fffffff` | Linux CMA pool (512 MiB)                |
| `0x30000000` - `0x307fffff` | Zephyr image and RAM                    |
| `0x30800000` - `0x3080ffff` | Zephyr console                          |
| `0x30900000` - `0x3090ffff` | rpmsg vrings                            |
| `0x30910000` - `0x309fffff` | rpmsg buffers (`shared-dma-pool`)       |

Linux sees `0x30000000` - `0x3090ffff` and the buffer pool as `no-map` reserved memory. The
firmware's devicetree asks for a 512 MiB CMA pool inside `0x0` - `0x30000000`; Zephyr sits just
above it, and U-Boot keeps the kernel, initrd and devicetree below `0x10000000` so the pool still
fits.

## How it works

The firmware parks cores 1-3 in a `wfe` loop polling their spin-table release addresses
(`0xd8`, `0xe0`, `0xe8`, `0xf0`). `uboot/boot.cmd` boots Linux with `nr_cpus=3`, which makes
core 3 impossible for Linux, so it never touches core 3's release address. (`maxcpus=3` is not
enough: Linux still prepares every possible CPU and core 3 would end up in the kernel's holding
pen.) The script also adds the reserved memory and a `zephyr-amp` node to the firmware's
devicetree, on a copy: `${fdt_addr}` is U-Boot's live control devicetree.

`linux/rproc/rpi4_amp_rproc.c` binds to `zephyr-amp`, loads the Zephyr ELF into the reserved
memory, writes its entry point to `0xf0` and wakes core 3 with `sev`. Zephyr comes up after
Linux has set up the GIC distributor, and `CONFIG_GIC_SAFE_CONFIG` makes it leave the distributor
as it is.

Notifications use two SPIs that no device drives, through the `zephyr,mbox-gic-sw` mailbox:
Linux sets SPI 220 pending for core 3, Zephyr sets SPI 221 pending for Linux. The shared memory is
non-cacheable on both sides.

## Zephyr side

`apps/rpmsg` is Zephyr's `openamp_rsc_table` sample (`src/main_remote.c`, copied with larger
thread stacks for arm64): it answers the kernel's `rpmsg_client_sample` and echoes what is written
to `/dev/ttyRPMSG0`. Its overlay moves RAM to `0x30000000`, adds the console and IPC regions and
disables every device that Linux owns (VideoCore mailbox and firmware, GPIO, UART, I2C, GENET,
USB, RNG); `prj.conf` turns off SMP, CPU power management, power domains and PCIe.
`CONFIG_RAM_CONSOLE` adds a trace entry to the resource table that does not build on 64-bit, so
`src/ram_log.c` writes the console region instead.

`zephyr/`, `drivers/`, `dts/` and `modules/` make this directory a Zephyr module carrying a copy
of the `zephyr,mbox-gic-sw` driver and binding (zephyrproject-rtos/zephyr#124185). The copy is
used only when the Zephyr tree lacks them; delete it once the pinned Zephyr has the driver.

    west build -b rpi_4b rpi4-amp/apps/rpmsg
    strip --strip-debug -o zephyr-amp.elf build/zephyr/zephyr.elf

remoteproc needs the section headers to find `.resource_table`, so strip only the debug
information. In a workspace that does not list them, add the modules by hand:
`-- -DEXTRA_ZEPHYR_MODULES="<ws>/modules/hal/cmsis;<ws>/modules/hal/libmetal;<ws>/modules/lib/open-amp;<ws>/rpi4-amp"`.

## Linux side

Target: Raspberry Pi OS (64-bit) with `kernel=u-boot.bin` in `config.txt`. The boot script loads
`kernel8.img` and `initramfs8` from the FAT partition (`mmc 0:1`) and uses the devicetree and
command line the firmware passes in `${fdt_addr}`.

    python3 scripts/mkscr.py uboot/boot.cmd boot.scr

Copy `boot.scr` and `zephyr-amp.elf` to the FAT partition, then at the U-Boot prompt:

    load mmc 0:1 ${scriptaddr} boot.scr; source ${scriptaddr}

`amp_extra_args` is appended to the kernel command line; for a root shell on the serial console
without a login: `setenv amp_extra_args "init=/bin/bash console=ttyS0,115200"`.

The Raspberry Pi OS kernel has no virtio, rpmsg or remoteproc. `linux/rproc/` builds them as
external modules from the kernel sources of the running version, plus the remoteproc driver.
Upstream remoteproc is built-in only; the two glue files split its virtio half into a module of
its own and export what it needs. On the Pi (headers and gcc come with Raspberry Pi OS):

    linux/rproc/get-sources.sh <linux-6.18.50 tree>
    make -C linux/rproc
    sudo linux/rproc/load.sh                  # zephyr-amp.elf in /boot/firmware
    dmesg | grep rpmsg_client_sample          # "incoming msg 100", "goodbye!"
    sudo python3 linux/ramcon.py              # Zephyr's console

## Limits

- No isolation: either side can overwrite the other's memory.
- Core 3 cannot be stopped (no PSCI on the Pi 4): stopping the remote fails, and restarting
  Zephyr needs a reboot.
- The console wraps after 64 KiB.
- The doorbell SPIs must stay unused by hardware; SPI 220/221 are above every SPI the Pi 4
  devicetree uses.

## License

The kernel code in `linux/rproc/` is GPL-2.0-only, as Linux requires for a module; the kernel
sources `get-sources.sh` copies keep their own licenses. `apps/rpmsg/src/main_remote.c` is
Zephyr's sample under its original copyright. Everything else is Apache-2.0.
