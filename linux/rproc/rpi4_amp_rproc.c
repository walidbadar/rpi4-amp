// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
/*
 * remoteproc driver for a Zephyr image on a Raspberry Pi 4 CPU that Linux
 * does not use.
 *
 * Linux must boot with nr_cpus excluding that CPU, and the memory below
 * must be a no-map reserved-memory region. The CPU is started through its
 * spin-table release address and cannot be stopped again.
 *
 * Notifications are two GIC SPIs that no device drives: kick_spi is set
 * pending for the remote CPU, the remote CPU sets the SPI in the node's
 * "interrupts" pending for Linux.
 *
 * Memory layout from base, matching the Zephyr devicetree:
 *   +0x000000  9 MiB   image, RAM and console
 *   +0x900000  32 KiB  vring 0
 *   +0x908000  32 KiB  vring 1
 *   +0x910000  960 KiB rpmsg buffers, the node's "memory-region"
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpumask.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <asm/barrier.h>

#include "remoteproc_internal.h"

#define AMP_IMAGE_SIZE		(SZ_8M + SZ_1M)
#define AMP_VRING_SIZE		SZ_32K
#define AMP_BUFFER_SIZE		(SZ_1M - 2 * AMP_VRING_SIZE)
#define AMP_MAPPED_SIZE		(AMP_IMAGE_SIZE + 2 * AMP_VRING_SIZE)

#define GIC_SPI_INTID_BASE	32
#define GICD_ISPENDR		0x200
#define GICD_ITARGETSR		0x800

static ulong base = 0x30000000;
module_param(base, ulong, 0444);
MODULE_PARM_DESC(base, "Start of the memory used by the remote CPU");

static uint hwid = 3;
module_param(hwid, uint, 0444);
MODULE_PARM_DESC(hwid, "MPIDR affinity of the remote CPU");

static char *firmware = "zephyr-amp.elf";
module_param(firmware, charp, 0444);
MODULE_PARM_DESC(firmware, "Default ELF image in the firmware search path");

static uint kick_spi = 220;
module_param(kick_spi, uint, 0444);
MODULE_PARM_DESC(kick_spi, "SPI set pending to notify the remote CPU");

struct amp_rproc {
	void __iomem *gicd;
	u64 release_addr;
};

static struct device_node *amp_find_cpu(void)
{
	struct device_node *np;
	u32 reg;

	for_each_of_cpu_node(np) {
		if (!of_property_read_u32(np, "reg", &reg) && reg == hwid)
			return np;
	}

	return NULL;
}

static int amp_get_release_addr(struct device *dev, u64 *release_addr)
{
	struct device_node *np, *cpu_np;
	const char *method;
	unsigned int cpu;
	int ret = 0;

	np = amp_find_cpu();
	if (!np) {
		dev_err(dev, "no cpu node with reg %u\n", hwid);
		return -ENODEV;
	}

	for_each_possible_cpu(cpu) {
		cpu_np = of_get_cpu_node(cpu, NULL);
		of_node_put(cpu_np);
		if (cpu_np == np) {
			dev_err(dev, "cpu %u belongs to Linux, boot with nr_cpus=%u\n", hwid, hwid);
			ret = -EBUSY;
			goto put;
		}
	}

	if (of_property_read_string(np, "enable-method", &method) ||
	    strcmp(method, "spin-table") ||
	    of_property_read_u64(np, "cpu-release-addr", release_addr)) {
		dev_err(dev, "cpu %u has no spin-table release address\n", hwid);
		ret = -EINVAL;
	}
put:
	of_node_put(np);
	return ret;
}

static int amp_mem_alloc(struct rproc *rproc, struct rproc_mem_entry *mem)
{
	/* The remote CPU maps this memory non-cacheable */
	mem->va = memremap(mem->dma, mem->len, MEMREMAP_WC);
	if (!mem->va)
		return -ENOMEM;

	return 0;
}

static int amp_mem_release(struct rproc *rproc, struct rproc_mem_entry *mem)
{
	memunmap(mem->va);

	return 0;
}

static int amp_add_carveout(struct rproc *rproc, phys_addr_t pa, size_t len, const char *name)
{
	struct rproc_mem_entry *mem;

	mem = rproc_mem_entry_init(&rproc->dev, NULL, pa, len, pa, amp_mem_alloc,
				   amp_mem_release, name);
	if (!mem)
		return -ENOMEM;

	rproc_add_carveout(rproc, mem);

	return 0;
}

static int amp_prepare(struct rproc *rproc)
{
	phys_addr_t shm = base + AMP_IMAGE_SIZE;
	struct rproc_mem_entry *mem;
	int ret;

	if (region_intersects(base, AMP_MAPPED_SIZE, IORESOURCE_SYSTEM_RAM,
			      IORES_DESC_NONE) != REGION_DISJOINT) {
		dev_err(&rproc->dev, "%#lx is Linux RAM, reserve it with no-map\n", base);
		return -EINVAL;
	}

	ret = amp_add_carveout(rproc, base, AMP_IMAGE_SIZE, "image");
	if (ret)
		return ret;

	ret = amp_add_carveout(rproc, shm, AMP_VRING_SIZE, "vdev0vring0");
	if (ret)
		return ret;

	ret = amp_add_carveout(rproc, shm + AMP_VRING_SIZE, AMP_VRING_SIZE, "vdev0vring1");
	if (ret)
		return ret;

	/* remoteproc makes memory-region 0 the vdev's coherent DMA pool */
	mem = rproc_of_resm_mem_entry_init(rproc->dev.parent, 0, AMP_BUFFER_SIZE,
					   shm + 2 * AMP_VRING_SIZE, "vdev0buffer");
	if (!mem)
		return -ENOMEM;

	rproc_add_carveout(rproc, mem);

	return 0;
}

static int amp_start(struct rproc *rproc)
{
	struct amp_rproc *amp = rproc->priv;
	u64 *slot;
	int ret = 0;

	slot = memremap(amp->release_addr, sizeof(*slot), MEMREMAP_WB);
	if (!slot)
		return -ENOMEM;

	if (READ_ONCE(*slot)) {
		dev_err(&rproc->dev, "cpu %u already released to %#llx\n", hwid, READ_ONCE(*slot));
		ret = -EBUSY;
		goto unmap;
	}

	/*
	 * Linux targets every SPI at its boot CPU and the remote OS only adds
	 * itself: deliver the kick to the remote CPU alone. GIC CPU interface n
	 * is core n on the BCM2711.
	 */
	writeb(BIT(hwid), amp->gicd + GICD_ITARGETSR + kick_spi + GIC_SPI_INTID_BASE);

	/* Image and resource table in memory before the CPU leaves its wfe loop */
	mb();
	WRITE_ONCE(*slot, rproc->bootaddr);
	asm volatile("dc civac, %0" : : "r"(slot) : "memory");
	dsb(sy);
	sev();

	dev_info(&rproc->dev, "released cpu %u to %#llx\n", hwid, rproc->bootaddr);
unmap:
	memunmap(slot);
	return ret;
}

static int amp_stop(struct rproc *rproc)
{
	dev_err(&rproc->dev, "cpu %u cannot be stopped\n", hwid);

	return -EOPNOTSUPP;
}

static void amp_kick(struct rproc *rproc, int vqid)
{
	struct amp_rproc *amp = rproc->priv;
	u32 intid = kick_spi + GIC_SPI_INTID_BASE;

	writel(BIT(intid % 32), amp->gicd + GICD_ISPENDR + (intid / 32) * 4);
}

static const struct rproc_ops amp_rproc_ops = {
	.prepare = amp_prepare,
	.start = amp_start,
	.stop = amp_stop,
	.kick = amp_kick,
	.load = rproc_elf_load_segments,
	.parse_fw = rproc_elf_load_rsc_table,
	.find_loaded_rsc_table = rproc_elf_find_loaded_rsc_table,
	.sanity_check = rproc_elf_sanity_check,
	.get_boot_addr = rproc_elf_get_boot_addr,
};

static int amp_vq_notify(int id, void *ptr, void *data)
{
	rproc_vq_interrupt(data, id);

	return 0;
}

static irqreturn_t amp_irq_thread(int irq, void *data)
{
	struct rproc *rproc = data;

	/* The doorbell carries no vring id: check all of them */
	idr_for_each(&rproc->notifyids, amp_vq_notify, rproc);

	return IRQ_HANDLED;
}

static int amp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *gic;
	struct amp_rproc *amp;
	struct rproc *rproc;
	int irq, ret;

	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	rproc = devm_rproc_alloc(dev, "zephyr", &amp_rproc_ops, firmware, sizeof(*amp));
	if (!rproc)
		return -ENOMEM;

	rproc->auto_boot = false;
	amp = rproc->priv;

	ret = amp_get_release_addr(dev, &amp->release_addr);
	if (ret)
		return ret;

	gic = of_find_compatible_node(NULL, NULL, "arm,gic-400");
	if (!gic) {
		dev_err(dev, "no GIC-400\n");
		return -ENODEV;
	}

	amp->gicd = devm_of_iomap(dev, gic, 0, NULL);
	if (IS_ERR(amp->gicd)) {
		ret = PTR_ERR(amp->gicd);
		goto put;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto put;
	}

	ret = devm_request_threaded_irq(dev, irq, NULL, amp_irq_thread, IRQF_ONESHOT,
					dev_name(dev), rproc);
	if (ret)
		goto put;

	ret = devm_rproc_add(dev, rproc);
put:
	of_node_put(gic);
	return ret;
}

static const struct of_device_id amp_of_match[] = {
	{ .compatible = "rpi4-amp,zephyr" },
	{ }
};
MODULE_DEVICE_TABLE(of, amp_of_match);

static struct platform_driver amp_driver = {
	.probe = amp_probe,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = amp_of_match,
	},
};
module_platform_driver(amp_driver);

MODULE_DESCRIPTION("remoteproc driver for a Zephyr image on a Raspberry Pi 4 CPU");
MODULE_LICENSE("GPL");
