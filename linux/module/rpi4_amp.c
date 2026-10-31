// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
/*
 * Start a Zephyr image on a spin-table CPU that Linux does not use.
 *
 * Linux must boot with nr_cpus excluding that CPU, and the image memory
 * must be a no-map reserved-memory region. The CPU cannot be stopped
 * again: unloading the module leaves it running.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpumask.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <asm/barrier.h>

static ulong base = 0x30000000;
module_param(base, ulong, 0444);
MODULE_PARM_DESC(base, "Load and entry address of the image");

static ulong size = SZ_8M;
module_param(size, ulong, 0444);
MODULE_PARM_DESC(size, "Size of the memory reserved for the image");

static uint hwid = 3;
module_param(hwid, uint, 0444);
MODULE_PARM_DESC(hwid, "MPIDR affinity of the CPU to start");

static char *firmware = "zephyr-amp.bin";
module_param(firmware, charp, 0444);
MODULE_PARM_DESC(firmware, "Image file name in the firmware search path");

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

static bool amp_cpu_used_by_linux(struct device_node *np)
{
	struct device_node *cpu_np;
	unsigned int cpu;
	bool used = false;

	for_each_possible_cpu(cpu) {
		cpu_np = of_get_cpu_node(cpu, NULL);
		if (cpu_np == np)
			used = true;
		of_node_put(cpu_np);
	}

	return used;
}

static int amp_load(void)
{
	const struct firmware *fw;
	void *dst;
	int ret;

	ret = request_firmware(&fw, firmware, NULL);
	if (ret) {
		pr_err("cannot load %s: %d\n", firmware, ret);
		return ret;
	}

	if (fw->size > size) {
		pr_err("%s is %zu bytes, only %lu reserved\n", firmware, fw->size, size);
		ret = -EFBIG;
		goto out;
	}

	/* The CPU starts with its MMU and caches off: write around the caches */
	dst = memremap(base, fw->size, MEMREMAP_WC);
	if (!dst) {
		ret = -ENOMEM;
		goto out;
	}

	memcpy(dst, fw->data, fw->size);
	memunmap(dst);
	/* Image in memory before the release address is written */
	mb();

	pr_info("loaded %s (%zu bytes) at %#lx\n", firmware, fw->size, base);
out:
	release_firmware(fw);
	return ret;
}

static int __init amp_init(void)
{
	struct device_node *np;
	const char *method;
	u64 release_addr;
	u64 *slot;
	int ret;

	np = amp_find_cpu();
	if (!np) {
		pr_err("no cpu node with reg %u\n", hwid);
		return -ENODEV;
	}

	if (amp_cpu_used_by_linux(np)) {
		pr_err("cpu %u belongs to Linux, boot with nr_cpus=%u\n", hwid, hwid);
		ret = -EBUSY;
		goto put;
	}

	if (of_property_read_string(np, "enable-method", &method) ||
	    strcmp(method, "spin-table") ||
	    of_property_read_u64(np, "cpu-release-addr", &release_addr)) {
		pr_err("cpu %u has no spin-table release address\n", hwid);
		ret = -EINVAL;
		goto put;
	}

	if (region_intersects(base, size, IORESOURCE_SYSTEM_RAM, IORES_DESC_NONE) !=
	    REGION_DISJOINT) {
		pr_err("%#lx-%#lx is Linux RAM, reserve it with no-map\n", base, base + size - 1);
		ret = -EINVAL;
		goto put;
	}

	slot = memremap(release_addr, sizeof(*slot), MEMREMAP_WB);
	if (!slot) {
		ret = -ENOMEM;
		goto put;
	}

	if (READ_ONCE(*slot)) {
		pr_err("cpu %u already released to %#llx\n", hwid, READ_ONCE(*slot));
		ret = -EBUSY;
		goto unmap;
	}

	ret = amp_load();
	if (ret)
		goto unmap;

	WRITE_ONCE(*slot, base);
	asm volatile("dc civac, %0" : : "r"(slot) : "memory");
	dsb(sy);
	sev();

	pr_info("released cpu %u to %#lx\n", hwid, base);
unmap:
	memunmap(slot);
put:
	of_node_put(np);
	return ret;
}

static void __exit amp_exit(void)
{
	pr_info("cpu %u keeps running\n", hwid);
}

module_init(amp_init);
module_exit(amp_exit);

MODULE_DESCRIPTION("Start a Zephyr image on a spin-table CPU outside Linux");
MODULE_LICENSE("GPL");
