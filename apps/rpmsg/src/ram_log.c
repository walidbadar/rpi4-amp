/*
 * SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * printk into the zephyr,ram-console region, laid out like CONFIG_RAM_CONSOLE
 * so linux/ramcon.py reads it. CONFIG_RAM_CONSOLE itself is off: it adds a
 * trace entry to the OpenAMP resource table that does not build on 64-bit.
 */

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk-hooks.h>

#define RAM_LOG_NODE DT_CHOSEN(zephyr_ram_console)
#define RAM_LOG_SIZE DT_REG_SIZE(RAM_LOG_NODE)

static char *const ram_log = (char *)DT_REG_ADDR(RAM_LOG_NODE);

static int ram_log_out(int c)
{
	static size_t pos;

	ram_log[pos] = (char)c;
	pos = (pos + 1U) % (RAM_LOG_SIZE - 1U);

	return c;
}

static int ram_log_init(void)
{
	__printk_hook_install(ram_log_out);

	return 0;
}

SYS_INIT(ram_log_init, PRE_KERNEL_1, 0);
