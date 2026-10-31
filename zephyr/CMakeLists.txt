# SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: Apache-2.0

# Copy of the zephyr,mbox-gic-sw driver for Zephyr trees without it
if(NOT EXISTS ${ZEPHYR_BASE}/drivers/mbox/mbox_gic_sw.c)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../drivers drivers)
endif()
