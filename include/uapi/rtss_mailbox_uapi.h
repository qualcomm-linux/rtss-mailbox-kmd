/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * All rights reserved.
 *
 * NOTE: this driver is still being upstreamed. This uAPI is not yet frozen.
 * struct layout and IOCTL numbers may change before the driver is accepted
 * upstream. Do not treat the current layout as a stable ABI.
 */
#ifndef _UAPI_RTSS_MAILBOX_H
#define _UAPI_RTSS_MAILBOX_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * UAPI version — encoded as (MAJOR << 16 | MINOR).
 *
 * MAJOR bump: breaking change (field removed/reordered, IOCTL number changed).
 *             KMD rejects userspace with -EINVAL if major does not match.
 * MINOR bump: additive change (new field appended, new IOCTL added).
 *             Old userspace still works; new fields are zero-filled by KMD.
 */
#define RTSS_MB_UAPI_VERSION_MAJOR	1
#define RTSS_MB_UAPI_VERSION_MINOR	0
#define RTSS_MB_UAPI_VERSION \
	((RTSS_MB_UAPI_VERSION_MAJOR << 16) | RTSS_MB_UAPI_VERSION_MINOR)

#define RTSS_MB_IO_MAGIC	0x5AU		/* IOCTL magic */

/* Values for rtssmb_evfd_info.mode */
#define RTSS_MB_MODE_RX		0U
#define RTSS_MB_MODE_TX		1U

/*
 * Cache policy values for RTSS_MB_SET_MMAP_ATTR.
 *
 * Priority model (highest to lowest):
 *   1. O_SYNC or O_DSYNC on open() — forces NONCACHED, IOCTL override rejected.
 *   2. RTSS_MB_SET_MMAP_ATTR IOCTL  — allowed only if fd opened without O_SYNC/O_DSYNC.
 *   3. Default                      — NONCACHED (kernel default for no-map regions,
 *                                     consistent with /dev/mem phys_mem_access_prot).
 *
 * Values mirror the kernel memory model — no driver-specific encoding:
 *   NONCACHED    — pgprot_noncached:    strong ordering, no buffering.
 *   WRITECOMBINE — pgprot_writecombine: writes buffered/coalesced, reads uncached.
 *   DEVICE       — pgprot_device:       device memory semantics (arch-defined).
 */
#define RTSS_MB_CACHE_NONCACHED		0U
#define RTSS_MB_CACHE_WRITECOMBINE	1U
#define RTSS_MB_CACHE_DEVICE		2U

/**
 * struct rtssmb_region - Physical region descriptor returned by KMD.
 * @addr:   Physical address to pass to mmap(). KMD pre-adjusts this to the
 *          start of the active descriptor sub-region within the carveout.
 * @size:   Size in bytes to pass to mmap().
 * @offset: Reserved — always 0. KMD pre-adjusts @addr so UMD uses the
 *          mmap pointer directly as the descriptor base.
 */
struct rtssmb_region {
	__u64 addr;
	__u32 size;
	__u32 offset;
};

/**
 * struct rtssmb_evfd_info - Per-channel fields within rtssmb_devctl.
 * @event_fd:   eventfd for RX interrupt notification.
 * @signal_num: IPCC signal number.
 * @mode:       channel direction: RTSS_MB_MODE_RX or RTSS_MB_MODE_TX.
 * @client_id:  IPCC client ID this signal belongs to.
 * @sender:     IPCC sender client ID identifying which IPCC controller
 *              instance (ipcc1..ipcc4) routes this signal. Combine with
 *              @client_id and @signal_num to identify a channel
 *              across multiple IPCC controllers.
 * @prot:       IPCC protocol instance.
 */
struct rtssmb_evfd_info {
	__s32 event_fd;
	__u32 signal_num;
	__u32 mode;
	__u32 client_id;
	__u32 sender;
	__u32 prot;
};

/**
 * struct rtssmb_devctl - IOCTL payload for channel operations.
 * @size:         sizeof(struct rtssmb_devctl) at userspace build time.
 * @uapi_version: RTSS_MB_UAPI_VERSION encoded by userspace.
 * @ioctl_magic:  must equal RTSS_MB_IO_MAGIC.
 * @evfdinfo:     channel-specific data.
 *
 * Fields must only be appended at the end. Never remove, rename, or reorder.
 * Append → bump MINOR. Remove/reorder → bump MAJOR.
 */
struct rtssmb_devctl {
	__u32 size;
	__u32 uapi_version;
	__u32 ioctl_magic;
	struct rtssmb_evfd_info evfdinfo;
};

/* Channel IOCTLs — carry struct rtssmb_devctl (userspace → kernel only) */
#define RTSS_MB_SEND_INTERRUPT	_IOW(RTSS_MB_IO_MAGIC, 0x1, struct rtssmb_devctl)
#define RTSS_MB_SET_EVENT_FD	_IOW(RTSS_MB_IO_MAGIC, 0x2, struct rtssmb_devctl)
#define RTSS_MB_DIS_INTERRUPT	_IOW(RTSS_MB_IO_MAGIC, 0x3, struct rtssmb_devctl)

/*
 * Region IOCTLs — KMD returns struct rtssmb_region from DT carveouts.
 * KMD stages the region for the subsequent mmap() on the same fd.
 */
#define RTSS_MB_GET_MB_REGION	_IOR(RTSS_MB_IO_MAGIC,  0x4, struct rtssmb_region)
#define RTSS_MB_GET_OTA_REGION	_IOR(RTSS_MB_IO_MAGIC,  0x5, struct rtssmb_region)

/*
 * Cache policy override IOCTL — must be called after GET_*_REGION and before mmap().
 * Rejected if fd was opened with O_SYNC or O_DSYNC (those take highest priority).
 * Arg: __u32 RTSS_MB_CACHE_* value.
 */
#define RTSS_MB_SET_MMAP_ATTR	_IOW(RTSS_MB_IO_MAGIC,  0x6, __u32)

#endif /* _UAPI_RTSS_MAILBOX_H */
