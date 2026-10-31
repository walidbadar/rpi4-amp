// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
/*
 * remoteproc driver for a CPU of the host SoC that Linux does not use.
 *
 * The node names the CPU in "cpus", the memory in "memory-region" and two
 * GIC SPIs that no device drives in "interrupts": the remote CPU sets the
 * first pending for Linux, Linux sets the second pending for the remote CPU.
 * Carveouts are named after their reserved-memory nodes; "vdev0buffer" is
 * the rpmsg buffer pool and must be a shared-dma-pool.
 *
 * Linux must boot without the CPU (nr_cpus), and every region must be
 * no-map. The CPU is started through its spin-table release address and
 * cannot be stopped again. The interrupt controller must be a GICv2.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bits.h>
#include <linux/cpumask.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/string.h>
#include <asm/barrier.h>
#include <asm/cputype.h>

#include "remoteproc_internal.h"

#define GICD_ITARGETSR		0x800

struct cpu_rproc {
	void __iomem *gicd;
	int kick_irq;
	u64 mpidr;
	u64 release_addr;
};

static const struct of_device_id cpu_rproc_gicv2_match[] = {
	{ .compatible = "arm,gic-400" },
	{ .compatible = "arm,cortex-a15-gic" },
	{ .compatible = "arm,cortex-a9-gic" },
	{ .compatible = "arm,cortex-a7-gic" },
	{ }
};

static int cpu_rproc_parse_cpu(struct device *dev, struct cpu_rproc *priv)
{
	struct device_node *np, *cpu_np;
	const char *method;
	unsigned int cpu;
	int ret = 0;

	np = of_parse_phandle(dev->of_node, "cpus", 0);
	if (!np) {
		dev_err(dev, "no cpus property\n");
		return -EINVAL;
	}

	for_each_possible_cpu(cpu) {
		cpu_np = of_get_cpu_node(cpu, NULL);
		of_node_put(cpu_np);
		if (cpu_np == np) {
			dev_err(dev, "%pOF belongs to Linux, boot with a lower nr_cpus\n", np);
			ret = -EBUSY;
			goto put;
		}
	}

	if (of_property_read_reg(np, 0, &priv->mpidr, NULL)) {
		dev_err(dev, "%pOF has no reg\n", np);
		ret = -EINVAL;
		goto put;
	}

	if (of_property_read_string(np, "enable-method", &method) ||
	    strcmp(method, "spin-table") ||
	    of_property_read_u64(np, "cpu-release-addr", &priv->release_addr)) {
		dev_err(dev, "%pOF has no spin-table release address\n", np);
		ret = -EINVAL;
	}
put:
	of_node_put(np);
	return ret;
}

static int cpu_rproc_mem_alloc(struct rproc *rproc, struct rproc_mem_entry *mem)
{
	if (region_intersects(mem->dma, mem->len, IORESOURCE_SYSTEM_RAM,
			      IORES_DESC_NONE) != REGION_DISJOINT) {
		dev_err(&rproc->dev, "%s is Linux RAM, reserve it with no-map\n", mem->name);
		return -EINVAL;
	}

	/* The remote CPU maps this memory non-cacheable */
	mem->va = memremap(mem->dma, mem->len, MEMREMAP_WC);
	if (!mem->va)
		return -ENOMEM;

	return 0;
}

static int cpu_rproc_mem_release(struct rproc *rproc, struct rproc_mem_entry *mem)
{
	memunmap(mem->va);

	return 0;
}

static int cpu_rproc_prepare(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	struct of_phandle_iterator it;
	struct rproc_mem_entry *mem;
	struct reserved_mem *rmem;
	int index = 0;

	of_phandle_iterator_init(&it, dev->of_node, "memory-region", NULL, 0);
	while (of_phandle_iterator_next(&it) == 0) {
		rmem = of_reserved_mem_lookup(it.node);
		if (!rmem) {
			dev_err(dev, "%pOF is not reserved memory\n", it.node);
			of_node_put(it.node);
			return -EINVAL;
		}

		/* remoteproc makes vdev0buffer the vdev's coherent DMA pool */
		if (strcmp(it.node->name, "vdev0buffer"))
			mem = rproc_mem_entry_init(dev, NULL, rmem->base, rmem->size, rmem->base,
						   cpu_rproc_mem_alloc, cpu_rproc_mem_release,
						   it.node->name);
		else
			mem = rproc_of_resm_mem_entry_init(dev, index, rmem->size, rmem->base,
							   it.node->name);

		if (!mem) {
			of_node_put(it.node);
			return -ENOMEM;
		}

		rproc_add_carveout(rproc, mem);
		index++;
	}

	return 0;
}

static int cpu_rproc_start(struct rproc *rproc)
{
	struct cpu_rproc *priv = rproc->priv;
	irq_hw_number_t kick_intid;
	u64 *slot;
	int ret = 0;

	slot = memremap(priv->release_addr, sizeof(*slot), MEMREMAP_WB);
	if (!slot)
		return -ENOMEM;

	if (READ_ONCE(*slot)) {
		dev_err(&rproc->dev, "cpu %#llx already released to %#llx\n", priv->mpidr,
			READ_ONCE(*slot));
		ret = -EBUSY;
		goto unmap;
	}

	/*
	 * Linux targets every SPI at its boot CPU and the remote OS only adds
	 * itself: deliver the kick to the remote CPU alone. The GIC CPU
	 * interface number is taken to be MPIDR Aff0.
	 */
	kick_intid = irqd_to_hwirq(irq_get_irq_data(priv->kick_irq));
	writeb(BIT(MPIDR_AFFINITY_LEVEL(priv->mpidr, 0)),
	       priv->gicd + GICD_ITARGETSR + kick_intid);

	/* Image and resource table in memory before the CPU leaves its wfe loop */
	mb();
	WRITE_ONCE(*slot, rproc->bootaddr);
	asm volatile("dc civac, %0" : : "r"(slot) : "memory");
	dsb(sy);
	sev();

	dev_info(&rproc->dev, "released cpu %#llx to %#llx\n", priv->mpidr, rproc->bootaddr);
unmap:
	memunmap(slot);
	return ret;
}

static int cpu_rproc_stop(struct rproc *rproc)
{
	dev_err(&rproc->dev, "a spin-table CPU cannot be stopped\n");

	return -EOPNOTSUPP;
}

static void cpu_rproc_kick(struct rproc *rproc, int vqid)
{
	struct cpu_rproc *priv = rproc->priv;

	irq_set_irqchip_state(priv->kick_irq, IRQCHIP_STATE_PENDING, true);
}

static const struct rproc_ops cpu_rproc_ops = {
	.prepare = cpu_rproc_prepare,
	.start = cpu_rproc_start,
	.stop = cpu_rproc_stop,
	.kick = cpu_rproc_kick,
	.load = rproc_elf_load_segments,
	.parse_fw = rproc_elf_load_rsc_table,
	.find_loaded_rsc_table = rproc_elf_find_loaded_rsc_table,
	.sanity_check = rproc_elf_sanity_check,
	.get_boot_addr = rproc_elf_get_boot_addr,
};

static int cpu_rproc_vq_notify(int id, void *ptr, void *data)
{
	rproc_vq_interrupt(data, id);

	return 0;
}

static irqreturn_t cpu_rproc_irq_thread(int irq, void *data)
{
	struct rproc *rproc = data;

	/* The doorbell carries no vring id: check all of them */
	idr_for_each(&rproc->notifyids, cpu_rproc_vq_notify, rproc);

	return IRQ_HANDLED;
}

static int cpu_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *gic;
	struct cpu_rproc *priv;
	const char *firmware;
	struct rproc *rproc;
	int irq, ret;

	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	ret = rproc_of_parse_firmware(dev, 0, &firmware);
	if (ret)
		firmware = NULL;

	rproc = devm_rproc_alloc(dev, dev->of_node->name, &cpu_rproc_ops, firmware,
				 sizeof(*priv));
	if (!rproc)
		return -ENOMEM;

	rproc->auto_boot = false;
	priv = rproc->priv;

	ret = cpu_rproc_parse_cpu(dev, priv);
	if (ret)
		return ret;

	gic = of_irq_find_parent(dev->of_node);
	if (!gic || !of_match_node(cpu_rproc_gicv2_match, gic)) {
		dev_err(dev, "interrupt parent is not a GICv2\n");
		ret = -ENODEV;
		goto put;
	}

	priv->gicd = devm_of_iomap(dev, gic, 0, NULL);
	if (IS_ERR(priv->gicd)) {
		ret = PTR_ERR(priv->gicd);
		goto put;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto put;
	}

	priv->kick_irq = platform_get_irq(pdev, 1);
	if (priv->kick_irq < 0) {
		ret = priv->kick_irq;
		goto put;
	}

	ret = devm_request_threaded_irq(dev, irq, NULL, cpu_rproc_irq_thread, IRQF_ONESHOT,
					dev_name(dev), rproc);
	if (ret)
		goto put;

	ret = devm_rproc_add(dev, rproc);
put:
	of_node_put(gic);
	return ret;
}

static const struct of_device_id cpu_rproc_of_match[] = {
	{ .compatible = "amp,cpu-rproc" },
	{ }
};
MODULE_DEVICE_TABLE(of, cpu_rproc_of_match);

static struct platform_driver cpu_rproc_driver = {
	.probe = cpu_rproc_probe,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = cpu_rproc_of_match,
	},
};
module_platform_driver(cpu_rproc_driver);

MODULE_DESCRIPTION("remoteproc driver for a host CPU that Linux does not use");
MODULE_LICENSE("GPL");
