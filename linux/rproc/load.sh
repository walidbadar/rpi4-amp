#!/bin/sh
# SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: GPL-2.0-only
#
# Load the modules built here in dependency order and start the remote CPU.
#
# usage: load.sh [module dir] [firmware dir]

set -e

dir=${1:-$(dirname "$0")}
fw=${2:-/boot/firmware}

for m in virtio_ring virtio_core rpmsg_core rpmsg_ns virtio_rpmsg_bus rpmsg_char rpmsg_ctrl \
	 rpmsg_tty rpmsg_client_sample remoteproc rproc_virtio cpu_rproc; do
	insmod "$dir/$m.ko"
done

printf '%s' "$fw" > /sys/module/firmware_class/parameters/path
echo start > /sys/class/remoteproc/remoteproc0/state
