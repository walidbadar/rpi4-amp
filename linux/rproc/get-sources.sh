#!/bin/sh
# SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: GPL-2.0-only
#
# Copy the virtio, rpmsg and remoteproc sources this directory builds from a
# Linux tree of the running kernel's version.
#
# usage: get-sources.sh <linux source tree>

set -e

src=$1
dst=$(dirname "$0")

if [ ! -f "$src/drivers/remoteproc/remoteproc_core.c" ]; then
	echo "usage: $0 <linux source tree>" >&2
	exit 1
fi

cp "$src"/drivers/virtio/virtio.c "$src"/drivers/virtio/virtio_anchor.c \
   "$src"/drivers/virtio/virtio_ring.c "$dst"
cp "$src"/drivers/rpmsg/rpmsg_core.c "$src"/drivers/rpmsg/rpmsg_internal.h \
   "$src"/drivers/rpmsg/rpmsg_ns.c "$src"/drivers/rpmsg/rpmsg_char.c \
   "$src"/drivers/rpmsg/rpmsg_char.h "$src"/drivers/rpmsg/rpmsg_ctrl.c \
   "$src"/drivers/rpmsg/virtio_rpmsg_bus.c "$dst"
cp "$src"/drivers/remoteproc/remoteproc_*.c "$src"/drivers/remoteproc/remoteproc_*.h "$dst"
rm -f "$dst"/remoteproc_cdev.c
cp "$src"/samples/rpmsg/rpmsg_client_sample.c "$src"/drivers/tty/rpmsg_tty.c "$dst"
