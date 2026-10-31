// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
/*
 * Module information for the remoteproc virtio driver, which upstream is
 * linked into the built-in remoteproc core.
 *
 * dma_declare_coherent_memory() and dma_release_coherent_memory() are not
 * exported to modules; Kbuild points remoteproc_virtio.c at these instead.
 * A vdev buffer pool must therefore come from a reserved-memory node: such a
 * device never has declared memory, so there is nothing to release.
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/types.h>

int rproc_virtio_declare_coherent_memory(struct device *dev, phys_addr_t phys_addr,
					 dma_addr_t device_addr, size_t size);
void rproc_virtio_release_coherent_memory(struct device *dev);

int rproc_virtio_declare_coherent_memory(struct device *dev, phys_addr_t phys_addr,
					 dma_addr_t device_addr, size_t size)
{
	dev_err(dev, "vdev buffer pool needs a reserved-memory node\n");

	return -EOPNOTSUPP;
}

void rproc_virtio_release_coherent_memory(struct device *dev)
{
}

MODULE_DESCRIPTION("Remote processor virtio device driver");
MODULE_LICENSE("GPL");
