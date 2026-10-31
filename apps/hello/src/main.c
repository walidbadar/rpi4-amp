/*
 * SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/arch/arm64/lib_helpers.h>

int main(void)
{
	uint32_t count = 0U;

	printk("Zephyr AMP on MPIDR 0x%llx\n", read_mpidr_el1());

	while (true) {
		printk("alive %u, uptime %lld ms\n", count++, k_uptime_get());
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
