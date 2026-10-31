<!--
SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
SPDX-License-Identifier: Apache-2.0
-->

# Linux and Zephyr AMP on the Raspberry Pi 4

Linux runs on cores 0-2, Zephyr on core 3. No hypervisor: both share the GIC-400 and the
memory system, and each owns its own peripherals.

## Memory map

| Range                       | Owner                                   |
|-----------------------------|-----------------------------------------|
| `0x00200000` - `0x0fffffff` | Linux kernel, initrd and devicetree     |
| `0x10000000` - `0x2fffffff` | Linux CMA pool (512 MiB)                |
| `0x30000000` - `0x307fffff` | Zephyr image and RAM                    |
| `0x30800000` - `0x3080ffff` | Zephyr RAM console                      |
| `0x30900000` - `0x3090ffff` | rpmsg vrings                            |
| `0x30910000` - `0x309fffff` | rpmsg buffers (`shared-dma-pool`)       |

Linux sees `0x30000000` - `0x3090ffff` and the buffer pool as `no-map` reserved memory.

The firmware's devicetree asks for a 512 MiB CMA pool inside `0x0` - `0x30000000`; Zephyr sits
just above it, and U-Boot keeps the kernel, initrd and devicetree below `0x10000000` so the pool
still fits.

## How core 3 starts

The firmware parks cores 1-3 in a `wfe` loop polling their spin-table release addresses
(`0xd8`, `0xe0`, `0xe8`, `0xf0`). Core 3 is released by U-Boot before Linux boots, by the
`rpi4_amp` kernel module once Linux is up, or by remoteproc (see rpmsg below).

### From U-Boot

`uboot/boot.cmd`:

1. loads `zephyr-amp.bin` to `0x30000000`, clears the RAM console and writes `0x30000000` to
   `0xf0`,
2. copies the firmware's devicetree (it is also U-Boot's live control devicetree, so it must not
   be edited in place) and adds the reserved-memory nodes and the `zephyr-amp` node for
   remoteproc,
3. appends `nr_cpus=3` to the kernel command line and boots Linux.

`nr_cpus=3` makes core 3 impossible for Linux, so it never prepares core 3's release address.
(`maxcpus=3` is not enough: Linux still prepares every possible CPU and core 3 would end up in
the kernel's holding pen.) Core 3 wakes on the `sev` Linux issues while releasing cpu1, which
happens after Linux has set up the GIC distributor. `CONFIG_GIC_SAFE_CONFIG` then makes Zephyr
leave the distributor as it is.

### From Linux

With `setenv amp_linux_start 1` the script still reserves the memory and limits Linux to three
CPUs, but leaves core 3 parked. `linux/module/rpi4_amp.c` then:

1. finds the CPU node with `reg = <3>` and refuses if Linux owns it or if it has no spin-table
   release address,
2. refuses if the load region is Linux RAM (the `no-map` node is missing) or if the CPU was
   already released,
3. loads `zephyr-amp.bin` with `request_firmware()` and copies it through a write-combining
   mapping (core 3 starts with its MMU and caches off),
4. writes the entry address to the release address, cleans it to the point of coherency and
   issues `sev`.

Unloading the module leaves Zephyr running; there is no way to stop core 3 again. Parameters:
`base`, `size`, `hwid`, `firmware`.

Build and start on the Pi (headers and gcc come with Raspberry Pi OS):

    make -C linux/module
    echo -n /boot/firmware | sudo tee /sys/module/firmware_class/parameters/path
    sudo insmod linux/module/rpi4_amp.ko

## rpmsg

`apps/rpmsg` is Zephyr's `openamp_rsc_table` sample (`src/main_remote.c`, copied with larger
thread stacks for arm64) on this board. Linux loads its ELF with remoteproc, and the two sides talk
rpmsg over virtio: the kernel's `rpmsg_client_sample` exchanges 100 messages with it, and
`/dev/ttyRPMSG0` echoes what is written to it.

Notifications use two SPIs that no device drives, through the `zephyr,mbox-gic-sw` mailbox:
Linux sets SPI 220 pending for core 3, Zephyr sets SPI 221 pending for Linux. The shared memory is non-cacheable on both sides. `CONFIG_RAM_CONSOLE` adds a trace entry to
Zephyr's resource table that does not build on 64-bit, so `src/ram_log.c` writes the same console
region instead.

The Raspberry Pi OS kernel has no virtio, rpmsg or remoteproc. `linux/rproc/` builds them as
external modules from the kernel sources of the running version, plus `rpi4_amp_rproc.c`, the
remoteproc driver for core 3. Upstream remoteproc is built-in only; the two glue files split its
virtio half into a module of its own and export what it needs. On the Pi:

    linux/rproc/get-sources.sh <linux-6.18.50 tree>
    make -C linux/rproc
    sudo linux/rproc/load.sh                  # needs zephyr-amp.elf in /boot/firmware
    dmesg | grep rpmsg_client_sample          # "incoming msg 100", "goodbye!"

Boot with `setenv amp_linux_start 1` so core 3 is still parked. Stopping the remote fails: core 3
cannot be stopped.

## Zephyr side

`zephyr/`, `drivers/`, `dts/` and `modules/` make this directory a Zephyr module. It carries a
copy of the `zephyr,mbox-gic-sw` driver and binding, proposed upstream, and uses them only when the
Zephyr tree lacks them; delete the copy once the pinned Zephyr has the driver. The applications are
in `apps/`: `hello` prints a heartbeat, `rpmsg` is above. Both include
`common/rpi_4b_amp.dtsi`, which moves RAM to `0x30000000`, adds the RAM console region and disables
every device that Linux owns (VideoCore mailbox and firmware, GPIO, UART, I2C, GENET, USB, RNG),
and `common/amp.conf`, which turns off SMP, CPU power management, power domains and PCIe.

Build:

    west build -b rpi_4b rpi4-amp/apps/hello
    west build -b rpi_4b rpi4-amp/apps/rpmsg

In a workspace that does not list them, add the modules by hand:
`-- -DEXTRA_ZEPHYR_MODULES="<ws>/modules/hal/cmsis;<ws>/modules/hal/libmetal;<ws>/modules/lib/open-amp;<ws>/rpi4-amp"`.
For remoteproc, strip only the debug information (`strip --strip-debug`): it needs the section
headers to find `.resource_table`.

## Linux side

Target: Raspberry Pi OS (64-bit) with `kernel=u-boot.bin` in `config.txt`. The script loads
`kernel8.img`, `initramfs8` and `zephyr-amp.bin` from the FAT partition (`mmc 0:1`) and uses the
devicetree and command line the firmware passes in `${fdt_addr}`.

    python3 scripts/mkscr.py uboot/boot.cmd boot.scr

Copy `boot.scr` and `zephyr.bin` of `apps/hello` (as `zephyr-amp.bin`) to the FAT partition, then
at the U-Boot prompt:

    load mmc 0:1 ${scriptaddr} boot.scr; source ${scriptaddr}

To fetch Zephyr over TFTP instead, set `ipaddr`, `serverip` and `amp_tftp=1` first.
`amp_extra_args` is appended to the kernel command line; for a root shell on the serial console
without a login:

    setenv amp_extra_args "init=/bin/bash console=ttyS0,115200"

In Linux, check the split and read Zephyr's output:

    nproc                                 # 3
    ls /proc/device-tree/reserved-memory  # zephyr@30000000
    sudo python3 linux/ramcon.py          # "Zephyr AMP on MPIDR 0x80000003", "alive N, ..."

## Limits

- No isolation: either side can overwrite the other's memory.
- Core 3 cannot be stopped or restarted from Linux (no PSCI on the Pi 4); restarting Zephyr needs
  a reboot.
- The RAM console wraps after 64 KiB.
- The doorbell SPIs must stay unused by hardware; SPI 220/221 are above every SPI the Pi 4
  devicetree uses.

## License

The kernel code in `linux/module/` and `linux/rproc/` is GPL-2.0-only, as Linux requires for a
module; the kernel sources `get-sources.sh` copies keep their own licenses. `apps/rpmsg/src/
main_remote.c` is Zephyr's sample under its original copyright. Everything else is Apache-2.0.
