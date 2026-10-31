# SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: Apache-2.0

zephyr_library_named(rpi4_amp_mbox)
zephyr_library_sources_ifdef(CONFIG_MBOX_GIC_SW mbox_gic_sw.c)
