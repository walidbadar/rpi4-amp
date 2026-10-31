// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
/*
 * remoteproc is built-in only upstream. Built as modules, its virtio half
 * is a module of its own and needs these core functions exported.
 */

#include <linux/module.h>
#include <linux/remoteproc.h>

#include "remoteproc_internal.h"

EXPORT_SYMBOL_GPL(rproc_add_rvdev);
EXPORT_SYMBOL_GPL(rproc_remove_rvdev);
EXPORT_SYMBOL_GPL(rproc_alloc_vring);
EXPORT_SYMBOL_GPL(rproc_free_vring);
EXPORT_SYMBOL_GPL(rproc_parse_vring);
EXPORT_SYMBOL_GPL(rproc_find_carveout_by_name);

MODULE_LICENSE("GPL");
