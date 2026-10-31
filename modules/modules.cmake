# SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: Apache-2.0

# Runs before devicetree processing: add the binding copy only for Zephyr
# trees without it, as two bindings for one compatible are an error
if(NOT EXISTS ${ZEPHYR_BASE}/dts/bindings/mbox/zephyr,mbox-gic-sw.yaml)
  list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
endif()
