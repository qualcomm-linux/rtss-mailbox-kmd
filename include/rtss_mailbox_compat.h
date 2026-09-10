/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * All rights reserved.
 *
 * Kernel API wrappers for rtss-mailbox-kmd.
 *
 * Add entries here when an API signature changes between kernel versions.
 * Use rtssmb_ prefix to distinguish wrappers from kernel APIs.
 * Driver code uses only the rtssmb_* wrappers — never calls kernel APIs directly.
 */
#ifndef RTSS_MAILBOX_COMPAT_H__
#define RTSS_MAILBOX_COMPAT_H__

#include <linux/device.h>
#include <linux/eventfd.h>
#include <linux/version.h>

/*
 * rtssmb_eventfd_signal - signal an eventfd.
 *
 *   kernel < 6.8: eventfd_signal(ctx, n)  — 2-arg form
 *   kernel >= 6.8: eventfd_signal(ctx)    — n argument removed
 *
 * Upstream change: commit f6be2662a2 ("eventfd: simplify eventfd_signal()")
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
static inline void rtssmb_eventfd_signal(struct eventfd_ctx *ctx)
{
	eventfd_signal(ctx);
}
#else
static inline void rtssmb_eventfd_signal(struct eventfd_ctx *ctx)
{
	eventfd_signal(ctx, 1);
}
#endif

/*
 * rtssmb_class_create - create a struct class.
 *
 *   kernel < 6.4: class_create(THIS_MODULE, name) — 2-arg form
 *   kernel >= 6.4: class_create(name)              — module arg removed
 *
 * Upstream change: commit 1aaba11da9 ("driver core: class: remove module *
 * from class_create()")
 *
 * QLI 6.6 already carries the 1-arg form. This wrapper documents the
 * boundary and allows painless backport to 6.1-era trees.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
static inline struct class *rtssmb_class_create(const char *name)
{
	return class_create(name);
}
#else
static inline struct class *rtssmb_class_create(const char *name)
{
	return class_create(THIS_MODULE, name);
}
#endif

/*
 * RTSSMB_REMOVE_RETURN_TYPE / RTSSMB_REMOVE_RETURN
 *   platform_driver.remove return type changed from int to void in kernel 6.11.
 *
 *   kernel <  6.11: .remove = int  (*)(struct platform_device *)  — return 0
 *   kernel >= 6.11: .remove = void (*)(struct platform_device *)  — no return
 *
 * Upstream change: commit 5c5a7680e67 ("platform: make platform_driver.remove()
 * return void")
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
#define RTSSMB_REMOVE_RETURN_TYPE	void
#define RTSSMB_REMOVE_RETURN
#else
#define RTSSMB_REMOVE_RETURN_TYPE	int
#define RTSSMB_REMOVE_RETURN		return 0
#endif

#endif /* RTSS_MAILBOX_COMPAT_H__ */
