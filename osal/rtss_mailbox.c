// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * All rights reserved.
 */
/* #define DEBUG */
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/eventfd.h>
#include <linux/interrupt.h>
#include <linux/mailbox_client.h>
#include <linux/mfd/syscon.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/suspend.h>
#include "rtss_mailbox_uapi.h"
#include "rtss_mailbox_compat.h"

#define RTSSMB_IPC_SIGNAL	BIT(23)
#define RTSSMB_S3_ACK_TIMEOUT_MS	2000
#define RTSSMB_S3_RETRY_INTERVAL_MS	100
#define RTSSMB_IPC_DEFAULT_DLY_US	50000

#define RTSSMB_HANDSHAKE_S1	0
#define RTSSMB_HANDSHAKE_S2	1
#define RTSSMB_HANDSHAKE_S3	2
#define RTSSMB_HANDSHAKE_MAGIC_COUNT	3

struct rtssmb_mmapinfo {
	phys_addr_t addr;
	size_t size;
};

struct rtssmb_irqinfo {
	unsigned int irq;
	unsigned int client_id;
	unsigned int signal_id;
	unsigned int sender;
	unsigned int prot;
	char *irq_name;
	struct eventfd_ctx *event_fd_sig;
	struct work_struct signal_work;
};

struct rtssmb_chaninfo {
	struct mbox_client client;
	struct mbox_chan *mchan;
	unsigned int client_id;
	unsigned int signal_id;
	unsigned int sender;
	unsigned int prot;
};

struct rtssmb_file_ctx {
	struct rtssmb_mmapinfo pr_data;     /* mmap addr/size staged by GET_MB/OTA_REGION */
	bool mmap_ioctl_set;                /* GET_*_REGION called, mmap() not yet done */
	u32 client_id;                      /* IPCC client registered via SET_EVENT_FD */
	u32 signal_num;                     /* IPCC signal registered via SET_EVENT_FD */
	u32 sender;                         /* IPCC controller instance registered via SET_EVENT_FD */
	u32 prot;                           /* IPCC protocol instance registered via SET_EVENT_FD */
	unsigned int irq;                   /* Linux IRQ for signal_num */
	bool irq_enabled;                   /* true after SET_EVENT_FD; cleared by DIS or .release */
	u32 cache_policy;                   /* RTSS_MB_CACHE_* — default NONCACHED */
	bool sync_locked;                   /* true if O_SYNC/O_DSYNC on open — IOCTL override rejected */
};

struct rtssmb_sender_lut_entry {
	phys_addr_t reg_base;
	unsigned int sender;
};

struct rtssmb_match_data {
	const u32 *handshake_magic;
	unsigned int handshake_delay_us;
	u32 handle_offset;
	u32 handle_size;
	const struct rtssmb_sender_lut_entry *sender_lut;
	int sender_lut_count;
	bool tcsr_word_packed;
};

struct rtssmb_handle {
	struct device *dev;
	struct cdev rtssmb_cdev;
	struct class *rtssmb_class;
	int rtssmb_devmaj;
	dev_t rtssmb_devnum;
	struct mutex dev_lock;
	spinlock_t irq_lock;
	struct rtssmb_chaninfo *chaninfo;
	struct rtssmb_irqinfo *irqinfo;
	unsigned int *irq_snapshot;         /* pre-allocated; used by suspend/resume */
	int mbox_count;
	int irq_count;
	const struct rtssmb_match_data *match_data;
	const struct rtssmb_sender_lut_entry *sender_lut;
	int sender_lut_count;
	struct regmap *req_tcsr_regmap;
	struct regmap *resp_tcsr_regmap;
	struct regmap *ipc_regmap;
	u32 req_tcsr_offset;
	u32 resp_tcsr_offset;
	u32 ipc_offset;
	bool tcsr_word_packed;
	unsigned int rtss_handshake_delay;
	phys_addr_t mb_addr;
	size_t      mb_size;
	u32         mb_offset;
	u32         mb_handle_size;
	phys_addr_t ota_addr;
	size_t      ota_size;
	u32         ota_offset;
};

static struct rtssmb_handle *rtssmb_ctx;

/**
 * rtssmb_sender_for_phandle() - Resolve a mboxes/interrupts-extended phandle to a sender.
 * @pdev:  Platform device (for error logging).
 * @node:  Target device_node the phandle at this DT entry resolved to.
 *
 * Looks up @node's reg base in rtssmb_ctx->sender_lut (match_data->sender_lut).
 *
 * Return: sender index (>= 0) on success, negative error if @node's reg
 * base is not a recognised RTSS-facing IPCC block.
 */
static int rtssmb_sender_for_phandle(struct platform_device *pdev, struct device_node *node)
{
	struct resource res;
	int i;

	if (of_address_to_resource(node, 0, &res)) {
		dev_err(&pdev->dev, "failed to read reg for %pOF\n", node);
		return -EINVAL;
	}

	for (i = 0; i < rtssmb_ctx->sender_lut_count; i++) {
		if (res.start == rtssmb_ctx->sender_lut[i].reg_base)
			return (int)rtssmb_ctx->sender_lut[i].sender;
	}

	dev_err(&pdev->dev, "%pOF reg base 0x%llx not a recognised RTSS IPCC sender\n",
		node, (unsigned long long)res.start);
	return -ENOENT;
}

/**
 * rtssmb_get_mchan_by_signal() - Look up mbox channel by (sender, client_id, signal_id).
 * @chaninfo:   Channel info array.
 * @mbox_count: Array size.
 * @sender:     IPCC controller instance.
 * @client:     IPCC client ID.
 * @sig:        IPCC signal ID. Only unique within one (sender, client) pair —
 *              multiple IPCC controllers (ipcc1..ipcc4) can reuse the same
 *              signal number for the same client, so all three fields must
 *              match together.
 *
 * Return: Matching mbox_chan, or NULL if not found.
 */
static struct mbox_chan *rtssmb_get_mchan_by_signal(const struct rtssmb_chaninfo *chaninfo,
			const int mbox_count, const unsigned int sender,
			const unsigned int client, const unsigned int sig)
{
	int i;

	for (i = 0; i < mbox_count; i++) {
		if (chaninfo[i].sender == sender && chaninfo[i].client_id == client &&
		    chaninfo[i].signal_id == sig)
			return chaninfo[i].mchan;
	}
	return NULL;
}

/**
 * rtssmb_send_interrupt() - Send an IPCC interrupt to RTSS.
 * @rtssmb_ctx: Driver context.
 * @sender:     IPCC controller instance of the TX channel.
 * @client:     IPCC client ID of the TX channel.
 * @sig:        IPCC signal ID of the TX channel.
 *
 * Return: 0 on success, negative error on failure.
 */
static int rtssmb_send_interrupt(struct rtssmb_handle *rtssmb_ctx, const unsigned int sender,
	const unsigned int client, const unsigned int sig)
{
	int ret;
	struct mbox_chan *mchan;

	mchan = rtssmb_get_mchan_by_signal(rtssmb_ctx->chaninfo, rtssmb_ctx->mbox_count,
		sender, client, sig);
	if (!mchan) {
		dev_err(rtssmb_ctx->dev,
			"unable to find mchan corresponding to sender %u client %u signal %u\n",
			sender, client, sig);
		return -ENODEV;
	}

	ret = mbox_send_message(mchan, NULL);
	if (ret < 0) {
		dev_err(rtssmb_ctx->dev, "mbox send message failed for sender %u client %u signal %u: %d\n",
			sender, client, sig, ret);
		return ret;
	}

	mbox_client_txdone(mchan, 0);
	return 0;
}

/**
 * rtssmb_get_irq_by_signal() - Look up IRQ number by (sender, client_id, signal_id).
 * @irqinfo:   IRQ info array.
 * @irq_count: Array size.
 * @sender:    IPCC controller instance (which ipccN this RX channel is bookkept under).
 * @client:    IPCC client ID.
 * @signal:    IPCC signal ID.
 * @irq:       Output — Linux IRQ number on success.
 *
 * Return: 0 on success, -ENOENT if not found.
 */
static int rtssmb_get_irq_by_signal(const struct rtssmb_irqinfo *irqinfo, const int irq_count,
	unsigned int sender, unsigned int client, unsigned int signal, unsigned int *irq)
{
	int i;

	for (i = 0; i < irq_count; i++) {
		if (irqinfo[i].sender == sender && irqinfo[i].client_id == client &&
		    irqinfo[i].signal_id == signal) {
			*irq = irqinfo[i].irq;
			return 0;
		}
	}
	return -ENOENT;
}

/**
 * rtssmb_set_eventfd() - Associate an eventfd with an RX IPCC signal.
 * @irqinfo:   IRQ info array.
 * @irq_count: Array size.
 * @sender:    IPCC controller instance.
 * @client:    IPCC client ID.
 * @signal:    IPCC signal ID.
 * @event_fd:  Userspace eventfd file descriptor.
 *
 * Acquires eventfd reference and stores it under irq_lock for safe
 * access from the IRQ handler.
 *
 * Return: 0 on success, -ENOENT if signal not found, negative on bad fd.
 */
static int rtssmb_set_eventfd(struct rtssmb_irqinfo *irqinfo, const int irq_count,
	const unsigned int sender, const unsigned int client, const unsigned int signal,
	s32 *event_fd)
{
	struct eventfd_ctx *ctx;
	struct eventfd_ctx *old = NULL;
	unsigned long flags;
	int i;

	ctx = eventfd_ctx_fdget(*event_fd);
	if (IS_ERR(ctx))
		return (int)PTR_ERR(ctx);

	spin_lock_irqsave(&rtssmb_ctx->irq_lock, flags);
	for (i = 0; i < irq_count; i++) {
		if (irqinfo[i].sender == sender && irqinfo[i].client_id == client &&
		    irqinfo[i].signal_id == signal) {
			old = irqinfo[i].event_fd_sig;   /* capture before overwrite */
			irqinfo[i].event_fd_sig = ctx;
			spin_unlock_irqrestore(&rtssmb_ctx->irq_lock, flags);
			if (old)
				eventfd_ctx_put(old);        /* drop old ref outside lock */
			return 0;
		}
	}
	spin_unlock_irqrestore(&rtssmb_ctx->irq_lock, flags);
	eventfd_ctx_put(ctx);
	return -ENOENT;
}

/**
 * rtssmb_release_eventfd() - Release the eventfd for an RX signal.
 * @irqinfo:   IRQ info array.
 * @irq_count: Array size.
 * @sender:    IPCC controller instance.
 * @client:    IPCC client ID.
 * @signal:    IPCC signal ID.
 *
 * Clears the eventfd reference under irq_lock and drops the ctx refcount.
 *
 * Return: 0 on success, -ENOENT if not found or no eventfd registered.
 */
static int rtssmb_release_eventfd(struct rtssmb_irqinfo *irqinfo, const int irq_count,
	const unsigned int sender, const unsigned int client, const unsigned int signal)
{
	struct eventfd_ctx *ctx = NULL;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&rtssmb_ctx->irq_lock, flags);
	for (i = 0; i < irq_count; i++) {
		if (irqinfo[i].sender == sender && irqinfo[i].client_id == client &&
		    irqinfo[i].signal_id == signal && irqinfo[i].event_fd_sig) {
			ctx = irqinfo[i].event_fd_sig;
			irqinfo[i].event_fd_sig = NULL;
			break;
		}
	}
	spin_unlock_irqrestore(&rtssmb_ctx->irq_lock, flags);

	if (!ctx)
		return -ENOENT;

	eventfd_ctx_put(ctx);
	return 0;
}

/**
 * rtssmb_signal_work() - Workqueue handler: signal the registered eventfd.
 * @work: Embedded in rtssmb_irqinfo.
 *
 * Runs in process context. The ctx pointer is safe here because:
 * - .release and DIS_INTERRUPT both call disable_irq() which ensures the
 *   IRQ handler (and any scheduled work) has completed before they call
 *   rtssmb_release_eventfd() which drops the ctx.
 * - The work item is only scheduled when event_fd_sig is non-NULL; the
 *   pointer stays valid until after synchronize_irq() in the teardown path.
 */
static void rtssmb_signal_work(struct work_struct *work)
{
	struct rtssmb_irqinfo *info =
		container_of(work, struct rtssmb_irqinfo, signal_work);
	struct eventfd_ctx *ctx;
	unsigned long flags;

	spin_lock_irqsave(&rtssmb_ctx->irq_lock, flags);
	ctx = info->event_fd_sig;
	spin_unlock_irqrestore(&rtssmb_ctx->irq_lock, flags);

	if (ctx)
		rtssmb_eventfd_signal(ctx);
}

/**
 * rtssmb_intr_handler() - Top-half handler for incoming RTSS interrupts.
 * @irq:  IRQ number.
 * @data: Unused.
 *
 * Schedules rtssmb_signal_work to signal the eventfd from process context,
 * avoiding the need for an eventfd refcount increment in IRQ context.
 *
 * Return: IRQ_HANDLED, or IRQ_NONE if no eventfd is registered.
 */
static irqreturn_t rtssmb_intr_handler(int irq, void *data)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&rtssmb_ctx->irq_lock, flags);
	for (i = 0; i < rtssmb_ctx->irq_count; i++) {
		if (rtssmb_ctx->irqinfo[i].irq == irq) {
			if (rtssmb_ctx->irqinfo[i].event_fd_sig)
				schedule_work(&rtssmb_ctx->irqinfo[i].signal_work);
			spin_unlock_irqrestore(&rtssmb_ctx->irq_lock, flags);
			return IRQ_HANDLED;
		}
	}
	spin_unlock_irqrestore(&rtssmb_ctx->irq_lock, flags);

	dev_warn_ratelimited(rtssmb_ctx->dev,
		"IRQ %d fired with no handler registered\n", irq);
	return IRQ_NONE;
}

/**
 * rtssmb_copy_devctl() - Version-checked copy of devctl struct from userspace.
 * @dev:   Device for error logging.
 * @kdata: Output kernel buffer.
 * @arg:   Userspace pointer from ioctl arg.
 *
 * Copies min(user_size, kernel_size) bytes and zeroes remainder, allowing
 * UMD to work with KMD. Same pattern as perf_event_attr.
 *
 * Return: 0 on success, -EFAULT on bad pointer, -EINVAL on version/magic mismatch.
 */
static int rtssmb_copy_devctl(struct device *dev, struct rtssmb_devctl *kdata,
			      unsigned long arg)
{
	struct rtssmb_devctl __user *udata = (struct rtssmb_devctl __user *)arg;
	__u32 user_size;
	__u32 user_ver;
	size_t copy_size;

	if (get_user(user_size, &udata->size) ||
	    get_user(user_ver,  &udata->uapi_version))
		return -EFAULT;

	if ((user_ver >> 16) != RTSS_MB_UAPI_VERSION_MAJOR) {
		dev_err(dev, "UAPI major mismatch: kernel=%u userspace=%u\n",
			RTSS_MB_UAPI_VERSION_MAJOR, user_ver >> 16);
		return -EINVAL;
	}

	if (user_size < offsetof(struct rtssmb_devctl, evfdinfo)) {
		dev_err(dev, "devctl size %u too small\n", user_size);
		return -EINVAL;
	}

	memset(kdata, 0, sizeof(*kdata));
	copy_size = min_t(size_t, user_size, sizeof(*kdata));

	if (copy_from_user(kdata, udata, copy_size))
		return -EFAULT;

	if (kdata->ioctl_magic != RTSS_MB_IO_MAGIC) {
		dev_err(dev, "bad ioctl magic 0x%x\n", kdata->ioctl_magic);
		return -EINVAL;
	}

	return 0;
}

/**
 * rtssmb_open() - Allocate per-fd context.
 * @n: inode (unused).
 * @f: File handle.
 *
 * Return: 0 on success, -ENOMEM on allocation failure.
 */
static int rtssmb_open(struct inode *n, struct file *f)
{
	struct rtssmb_file_ctx *fctx = kzalloc(sizeof(*fctx), GFP_KERNEL);

	if (!fctx)
		return -ENOMEM;

	/* Default cache policy: noncached — kernel standard for no-map regions.
	 * Mirrors /dev/mem phys_mem_access_prot() behavior for memory above high_memory.
	 */
	fctx->cache_policy = RTSS_MB_CACHE_NONCACHED;

	/* O_SYNC or O_DSYNC on open — lock to noncached, reject IOCTL override.
	 * Consistent with kernel mem.c uncached_access() which checks O_DSYNC.
	 */
	fctx->sync_locked = !!(f->f_flags & (O_SYNC | O_DSYNC));

	f->private_data = fctx;
	return 0;
}

/**
 * rtssmb_release() - Free per-fd context; clean up IRQ and eventfd on crash.
 * @n: inode (unused).
 * @f: File handle.
 *
 * Called on fd close, process exit, or crash. If SET_EVENT_FD was called
 * without a matching DIS_INTERRUPT, disables the IRQ and drops the eventfd
 * reference. The mmap VMA is reclaimed by the kernel automatically.
 *
 * Return: 0.
 */
static int rtssmb_release(struct inode *n, struct file *f)
{
	struct rtssmb_file_ctx *fctx = f->private_data;
	bool do_cleanup = false;
	u32 client_id = 0;
	u32 signal_num = 0;
	u32 sender = 0;
	unsigned int irq = 0;

	if (!fctx)
		return 0;

	/* Atomically check and clear irq_enabled under dev_lock to prevent
	 * a race with concurrent DIS_INTERRUPT which also clears irq_enabled.
	 * Without this, both paths could call disable_irq + release_eventfd.
	 */
	mutex_lock(&rtssmb_ctx->dev_lock);
	if (fctx->irq_enabled) {
		do_cleanup        = true;
		client_id         = fctx->client_id;
		signal_num        = fctx->signal_num;
		sender            = fctx->sender;
		irq               = fctx->irq;
		fctx->irq_enabled = false;
	}
	mutex_unlock(&rtssmb_ctx->dev_lock);

	if (do_cleanup) {
		int i;

		disable_irq(irq);
		/* Flush any pending signal_work — the work reads event_fd_sig,
		 * which rtssmb_release_eventfd is about to free.
		 */
		for (i = 0; i < rtssmb_ctx->irq_count; i++) {
			if (rtssmb_ctx->irqinfo[i].irq == irq) {
				flush_work(&rtssmb_ctx->irqinfo[i].signal_work);
				break;
			}
		}
		rtssmb_release_eventfd(rtssmb_ctx->irqinfo,
				       rtssmb_ctx->irq_count,
				       sender,
				       client_id,
				       signal_num);
	}

	kfree(fctx);
	f->private_data = NULL;
	return 0;
}

/**
 * rtssmb_io_devctl() - unlocked_ioctl handler for /dev/rtssmb.
 * @f:   Open file handle.
 * @cmd: IOCTL command (RTSS_MB_*).
 * @arg: Userspace pointer to struct rtssmb_devctl.
 *
 * Return: 0 on success, -ENOTTY for unknown commands, negative error otherwise.
 */
static long rtssmb_io_devctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct rtssmb_file_ctx *fctx = f->private_data;
	int ret = 0;

	if (!fctx)
		return -ENXIO;

	switch (cmd) {
	case RTSS_MB_SEND_INTERRUPT: {
		struct rtssmb_devctl data;

		mutex_lock(&rtssmb_ctx->dev_lock);

		ret = rtssmb_copy_devctl(rtssmb_ctx->dev, &data, arg);
		if (ret) {
			dev_err(rtssmb_ctx->dev, "devctl copy failed cmd=%x ret=%d\n", cmd, ret);
			mutex_unlock(&rtssmb_ctx->dev_lock);
			return ret;
		}

		if (data.evfdinfo.mode == RTSS_MB_MODE_TX) {
			dev_dbg(rtssmb_ctx->dev, "sending irq sender %u client %u signal %u\n",
				data.evfdinfo.sender, data.evfdinfo.client_id, data.evfdinfo.signal_num);

			ret = rtssmb_send_interrupt(rtssmb_ctx, data.evfdinfo.sender,
				data.evfdinfo.client_id, data.evfdinfo.signal_num);
			if (ret < 0) {
				mutex_unlock(&rtssmb_ctx->dev_lock);
				return ret;
			}
		} else {
			ret = -EPERM;
			dev_err(rtssmb_ctx->dev, "send interrupt not permitted on RX channel\n");
		}

		mutex_unlock(&rtssmb_ctx->dev_lock);
		break;
	}

	case RTSS_MB_SET_EVENT_FD: {
		unsigned int irq;
		struct rtssmb_devctl data;

		mutex_lock(&rtssmb_ctx->dev_lock);

		ret = rtssmb_copy_devctl(rtssmb_ctx->dev, &data, arg);
		if (ret) {
			dev_err(rtssmb_ctx->dev, "devctl copy failed cmd=%x ret=%d\n", cmd, ret);
			mutex_unlock(&rtssmb_ctx->dev_lock);
			return ret;
		}

		if (data.evfdinfo.mode == RTSS_MB_MODE_RX) {
			if (fctx->irq_enabled) {
				dev_err(rtssmb_ctx->dev,
					"SET_EVENT_FD: irq already enabled on this fd\n");
				mutex_unlock(&rtssmb_ctx->dev_lock);
				return -EBUSY;
			}

			dev_dbg(rtssmb_ctx->dev, "setting eventfd for sender %u client %u signal %u\n",
				data.evfdinfo.sender, data.evfdinfo.client_id, data.evfdinfo.signal_num);

			ret = rtssmb_set_eventfd(rtssmb_ctx->irqinfo, rtssmb_ctx->irq_count,
				data.evfdinfo.sender, data.evfdinfo.client_id, data.evfdinfo.signal_num,
				&data.evfdinfo.event_fd);
			if (ret) {
				dev_err(rtssmb_ctx->dev, "set eventfd failed: %d\n", ret);
				mutex_unlock(&rtssmb_ctx->dev_lock);
				return ret;
			}

			ret = rtssmb_get_irq_by_signal(rtssmb_ctx->irqinfo, rtssmb_ctx->irq_count,
				data.evfdinfo.sender, data.evfdinfo.client_id, data.evfdinfo.signal_num, &irq);
			if (ret) {
				dev_err(rtssmb_ctx->dev, "irq lookup failed for sender %u client %u signal %u: %d\n",
					data.evfdinfo.sender, data.evfdinfo.client_id, data.evfdinfo.signal_num, ret);
				rtssmb_release_eventfd(rtssmb_ctx->irqinfo,
					rtssmb_ctx->irq_count,
					data.evfdinfo.sender,
					data.evfdinfo.client_id,
					data.evfdinfo.signal_num);
				mutex_unlock(&rtssmb_ctx->dev_lock);
				return ret;
			}

			enable_irq(irq);

			/* Record on this fd so .release can clean up on crash */
			fctx->client_id   = data.evfdinfo.client_id;
			fctx->signal_num  = data.evfdinfo.signal_num;
			fctx->sender      = data.evfdinfo.sender;
			fctx->prot        = data.evfdinfo.prot;
			fctx->irq         = irq;
			fctx->irq_enabled = true;
		} else {
			ret = -EPERM;
			dev_err(rtssmb_ctx->dev, "cannot set eventfd for TX channel\n");
		}

		mutex_unlock(&rtssmb_ctx->dev_lock);
		break;
	}

	case RTSS_MB_DIS_INTERRUPT: {
		unsigned int irq;
		struct rtssmb_devctl data;

		mutex_lock(&rtssmb_ctx->dev_lock);

		ret = rtssmb_copy_devctl(rtssmb_ctx->dev, &data, arg);
		if (ret) {
			dev_err(rtssmb_ctx->dev, "devctl copy failed cmd=%x ret=%d\n", cmd, ret);
			mutex_unlock(&rtssmb_ctx->dev_lock);
			return ret;
		}

		if (data.evfdinfo.mode == RTSS_MB_MODE_RX) {
			ret = rtssmb_get_irq_by_signal(rtssmb_ctx->irqinfo, rtssmb_ctx->irq_count,
				data.evfdinfo.sender, data.evfdinfo.client_id, data.evfdinfo.signal_num, &irq);
			if (ret) {
				dev_err(rtssmb_ctx->dev, "irq lookup failed for sender %u client %u signal %u: %d\n",
					data.evfdinfo.sender, data.evfdinfo.client_id, data.evfdinfo.signal_num, ret);
				mutex_unlock(&rtssmb_ctx->dev_lock);
				return ret;
			}

			/* Disable first — guarantees handler has exited and will not
			 * re-enter before we clear the eventfd pointer.
			 */
			disable_irq(irq);

			/* Flush any pending signal_work before releasing ctx. */
			{
				int j;

				for (j = 0; j < rtssmb_ctx->irq_count; j++) {
					if (rtssmb_ctx->irqinfo[j].irq == irq) {
						flush_work(&rtssmb_ctx->irqinfo[j].signal_work);
						break;
					}
				}
			}

			ret = rtssmb_release_eventfd(rtssmb_ctx->irqinfo,
				rtssmb_ctx->irq_count, data.evfdinfo.sender, data.evfdinfo.client_id,
				data.evfdinfo.signal_num);
			if (ret) {
				dev_err(rtssmb_ctx->dev, "release eventfd failed: %d\n", ret);
				enable_irq(irq);  /* undo disable — signal still registered */
				mutex_unlock(&rtssmb_ctx->dev_lock);
				return ret;
			}

			/* Clear per-fd flag — .release will skip cleanup */
			fctx->irq_enabled = false;
		} else {
			ret = -EPERM;
			dev_err(rtssmb_ctx->dev, "cannot disable interrupt for TX channel\n");
		}

		mutex_unlock(&rtssmb_ctx->dev_lock);
		break;
	}

	case RTSS_MB_GET_MB_REGION: {
		struct rtssmb_region __user *ureq = (struct rtssmb_region __user *)arg;
		struct rtssmb_region region;

		mutex_lock(&rtssmb_ctx->dev_lock);
		if (fctx->mmap_ioctl_set) {
			dev_err(rtssmb_ctx->dev, "GET_MB_REGION: mmap already pending\n");
			mutex_unlock(&rtssmb_ctx->dev_lock);
			return -EBUSY;
		}
		/* Return descriptor region only — addr adjusted by offset so UMD
		 * maps exactly the descriptor sub-region, not the full carveout.
		 * offset is 0: UMD uses mmap pointer directly as descriptor base.
		 * mb_handle_size == 0 is the match-data legacy choice:
		 * fall back to the derived mb_size - mb_offset.
		 */
		region.addr   = rtssmb_ctx->mb_addr + rtssmb_ctx->mb_offset;
		region.size   = rtssmb_ctx->mb_handle_size > 0 ?
				rtssmb_ctx->mb_handle_size :
				rtssmb_ctx->mb_size - rtssmb_ctx->mb_offset;
		region.offset = 0;
		fctx->pr_data.addr   = region.addr;
		fctx->pr_data.size   = region.size;
		fctx->mmap_ioctl_set = true;
		mutex_unlock(&rtssmb_ctx->dev_lock);
		if (copy_to_user(ureq, &region, sizeof(region))) {
			mutex_lock(&rtssmb_ctx->dev_lock);
			fctx->mmap_ioctl_set = false;
			mutex_unlock(&rtssmb_ctx->dev_lock);
			return -EFAULT;
		}
		break;
	}

	case RTSS_MB_GET_OTA_REGION: {
		struct rtssmb_region __user *ureq = (struct rtssmb_region __user *)arg;
		struct rtssmb_region region;

		mutex_lock(&rtssmb_ctx->dev_lock);
		if (fctx->mmap_ioctl_set) {
			dev_err(rtssmb_ctx->dev, "GET_OTA_REGION: mmap already pending\n");
			mutex_unlock(&rtssmb_ctx->dev_lock);
			return -EBUSY;
		}
		region.addr     = rtssmb_ctx->ota_addr;
		region.size     = rtssmb_ctx->ota_size;
		region.offset   = rtssmb_ctx->ota_offset;
		fctx->pr_data.addr   = rtssmb_ctx->ota_addr;
		fctx->pr_data.size   = rtssmb_ctx->ota_size;
		fctx->mmap_ioctl_set = true;
		mutex_unlock(&rtssmb_ctx->dev_lock);
		if (copy_to_user(ureq, &region, sizeof(region))) {
			mutex_lock(&rtssmb_ctx->dev_lock);
			fctx->mmap_ioctl_set = false;
			mutex_unlock(&rtssmb_ctx->dev_lock);
			return -EFAULT;
		}
		break;
	}

	case RTSS_MB_SET_MMAP_ATTR: {
		u32 policy;

		if (fctx->sync_locked) {
			dev_err(rtssmb_ctx->dev,
				"SET_MMAP_ATTR: rejected — fd opened with O_SYNC/O_DSYNC\n");
			return -EPERM;
		}
		if (get_user(policy, (__u32 __user *)arg))
			return -EFAULT;
		if (policy > RTSS_MB_CACHE_DEVICE) {
			dev_err(rtssmb_ctx->dev, "SET_MMAP_ATTR: invalid policy %u\n", policy);
			return -EINVAL;
		}
		mutex_lock(&rtssmb_ctx->dev_lock);
		fctx->cache_policy = policy;
		mutex_unlock(&rtssmb_ctx->dev_lock);
		break;
	}

	default:
		dev_err(rtssmb_ctx->dev, "invalid ioctl call, cmd=%x\n", cmd);
		ret = -ENOTTY;
	}
	return ret;
}

/**
 * rtssmb_mmap() - mmap handler to map the mailbox or OTA region to userspace.
 * @f:   Open file handle — private_data carries the per-fd rtssmb_file_ctx.
 * @vma: VMA descriptor for the requested mapping.
 *
 * Must be preceded by RTSS_MB_GET_MB_REGION or RTSS_MB_GET_OTA_REGION IOCTL
 * on the same fd, which stages the physical address in fctx->pr_data.
 *
 * Return: 0 on success, -EPROTO if no IOCTL preceded this call, negative error otherwise.
 */
static int rtssmb_mmap(struct file *f, struct vm_area_struct *vma)
{
	struct rtssmb_file_ctx *fctx = f->private_data;
	int ret;
	phys_addr_t phy_addr;
	size_t size;

	if (!fctx)
		return -ENXIO;

	mutex_lock(&rtssmb_ctx->dev_lock);

	if (!fctx->mmap_ioctl_set) {
		dev_err(rtssmb_ctx->dev, "mmap without prior SET_*_ADDR\n");
		mutex_unlock(&rtssmb_ctx->dev_lock);
		return -EPROTO;
	}

	phy_addr = fctx->pr_data.addr;
	size     = fctx->pr_data.size;

	if (!PAGE_ALIGNED(phy_addr)) {
		dev_err(rtssmb_ctx->dev, "mmap: physical address 0x%llx not page-aligned\n",
			(unsigned long long)phy_addr);
		fctx->mmap_ioctl_set = false;
		mutex_unlock(&rtssmb_ctx->dev_lock);
		return -EINVAL;
	}

	if ((vma->vm_end - vma->vm_start) > size) {
		dev_err(rtssmb_ctx->dev,
			"mmap: VMA size 0x%lx exceeds region size 0x%zx\n",
			vma->vm_end - vma->vm_start, size);
		fctx->mmap_ioctl_set = false;
		mutex_unlock(&rtssmb_ctx->dev_lock);
		return -EINVAL;
	}

	/* Use the VMA size — caller may map a sub-region of the physical window. */
	size = vma->vm_end - vma->vm_start;

	/* Apply cache policy — follows mem.c phys_mem_access_prot() model:
	 * no-map DDR is above high_memory → default noncached.
	 * O_SYNC/O_DSYNC locks to noncached (highest priority).
	 * RTSS_MB_SET_MMAP_ATTR overrides at runtime if fd opened without sync flags.
	 */
	switch (fctx->cache_policy) {
	case RTSS_MB_CACHE_WRITECOMBINE:
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
		break;
	case RTSS_MB_CACHE_DEVICE:
		vma->vm_page_prot = pgprot_device(vma->vm_page_prot);
		break;
	case RTSS_MB_CACHE_NONCACHED:
	default:
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		break;
	}

	/* remap_pfn_range sets VM_IO | VM_PFNMAP automatically.
	 * No explicit vm_flags_set needed — kernel handles all VM_ flags.
	 */
	ret = remap_pfn_range(vma, vma->vm_start,
			      PFN_DOWN(phy_addr), size, vma->vm_page_prot);

	/* Reset regardless of success/failure — prevents flag sticking on error */
	fctx->mmap_ioctl_set = false;

	if (ret)
		dev_err(rtssmb_ctx->dev, "remap_pfn_range failed: %d\n", ret);

	mutex_unlock(&rtssmb_ctx->dev_lock);
	return ret;
}

static const struct file_operations rtssmb_fops = {
	.open           = rtssmb_open,
	.release        = rtssmb_release,
	.unlocked_ioctl = rtssmb_io_devctl,
	.mmap           = rtssmb_mmap,
};

/**
 * rtssmb_write_tcsr_magic() - Write a magic word into a TCSR register.
 * @tcsr_regmap: qcom,syscon-tcsr-req/qcom,syscon-tcsr-resp regmap for this register.
 * @tcsr_offset: Offset of the TCSR request/response register(s) from
 *               tcsr_regmap's base.
 * @magic:       Magic word for the handshake stage (RTSSMB_S1/S2/S3_READY).
 *
 * split the 32-bit magic across 4 byte-wide registers at
 * tcsr_offset+0/4/8/12 (tcsr_word_packed == false); take
 * the full 32-bit magic in a single register at tcsr_offset
 * (tcsr_word_packed == true). Mirrors rtssmb_read_tcsr_magic()'s packing.
 */
static void rtssmb_write_tcsr_magic(struct regmap *tcsr_regmap, u32 tcsr_offset,
		unsigned int magic)
{
	int tcsr_write_value;
	unsigned int i;

	if (rtssmb_ctx->tcsr_word_packed) {
		regmap_write(tcsr_regmap, tcsr_offset, magic);
	} else {
		for (i = 0; i < 4; i++) {
			tcsr_write_value = (((magic & (0xFFU << (8U * i))) >> (8U * i)) & 0xFFU);
			regmap_write(tcsr_regmap, tcsr_offset + i * 4, tcsr_write_value);
		}
	}
}

/**
 * rtssmb_read_tcsr_magic() - Read the current magic word out of a TCSR register.
 * @tcsr_regmap: qcom,syscon-tcsr-req/qcom,syscon-tcsr-resp regmap for this register.
 * @tcsr_offset: Offset of the TCSR register(s) from tcsr_regmap's base.
 *
 * Mirrors rtssmb_write_tcsr_magic()'s packing: one 32-bit read when
 * tcsr_word_packed, or 4 byte-wide reads reassembled LSB-first otherwise.
 *
 * Return: the 32-bit magic word currently latched in the register(s).
 */
static unsigned int rtssmb_read_tcsr_magic(struct regmap *tcsr_regmap, u32 tcsr_offset)
{
	unsigned int val;
	unsigned int magic = 0U;

	if (rtssmb_ctx->tcsr_word_packed) {
		regmap_read(tcsr_regmap, tcsr_offset, &magic);
	} else {
		regmap_read(tcsr_regmap, tcsr_offset, &val);
		magic |= val & 0xFFU;
		regmap_read(tcsr_regmap, tcsr_offset + 4, &val);
		magic |= (val & 0xFFU) << 8U;
		regmap_read(tcsr_regmap, tcsr_offset + 8, &val);
		magic |= (val & 0xFFU) << 16U;
		regmap_read(tcsr_regmap, tcsr_offset + 12, &val);
		magic |= (val & 0xFFU) << 24U;
	}

	return magic;
}

/**
 * rtssmb_notify_rtss() - Trigger the IPC signal that wakes RTSS up.
 * @ipc_regmap: qcom,syscon-ipc regmap.
 * @ipc_offset: Offset of the IPC trigger register from ipc_regmap's base.
 *
 * Must be called after rtssmb_write_tcsr_magic() so RTSS observes the new
 * TCSR value once it services the IPC interrupt.
 */
static void rtssmb_notify_rtss(struct regmap *ipc_regmap, u32 ipc_offset)
{
	isb();
	/* ensure TCSR register writes go through before triggering IPC to RTSS */
	mb();

	regmap_write(ipc_regmap, ipc_offset, RTSSMB_IPC_SIGNAL);
}

/**
 * rtssmb_send_handshake() - Write the magic word to TCSR and notify RTSS.
 * @tcsr_regmap: qcom,syscon-tcsr-req/qcom,syscon-tcsr-resp regmap (see
 *               rtssmb_write_tcsr_magic()).
 * @tcsr_offset: TCSR request/response register offset (see
 *               rtssmb_write_tcsr_magic()).
 * @magic:       Magic word for the handshake stage (RTSSMB_S1/S2/S3_READY).
 */
static void rtssmb_send_handshake(struct regmap *tcsr_regmap, u32 tcsr_offset,
		unsigned int magic)
{
	rtssmb_write_tcsr_magic(tcsr_regmap, tcsr_offset, magic);
	rtssmb_notify_rtss(rtssmb_ctx->ipc_regmap, rtssmb_ctx->ipc_offset);
}

/**
 * rtssmb_init_handshakes() - Perform the S1→delay→S2 boot handshake with RTSS.
 *
 * Called at probe time and on resume. Sends rtssmb_ctx->match_data->handshake_magic[S1], waits for
 * rtss_handshake_delay microseconds, then sends rtssmb_ctx->match_data->handshake_magic[S2].
 *
 * Return: 0 on success, negative error if either handshake step fails.
 */
static int rtssmb_init_handshakes(void)
{
	unsigned int delay = rtssmb_ctx->rtss_handshake_delay;

	/* Clamp here so resume/error-recovery paths are also protected. */
	if (delay < 100)
		delay = 100;

	dev_dbg(rtssmb_ctx->dev, "sending s1\n");
	rtssmb_send_handshake(rtssmb_ctx->req_tcsr_regmap, rtssmb_ctx->req_tcsr_offset,
			rtssmb_ctx->match_data->handshake_magic[RTSSMB_HANDSHAKE_S1]);

	if (delay <= 20000)
		usleep_range(delay - 100, delay);
	else
		msleep(delay / 1000);

	dev_dbg(rtssmb_ctx->dev, "sending s2\n");
	rtssmb_send_handshake(rtssmb_ctx->req_tcsr_regmap, rtssmb_ctx->req_tcsr_offset,
			rtssmb_ctx->match_data->handshake_magic[RTSSMB_HANDSHAKE_S2]);

	return 0;
}

/**
 * rtssmb_suspend_handshake() - Notify RTSS of impending suspend and wait for ACK.
 *
 * Resends S3_READY every RTSSMB_S3_RETRY_INTERVAL_MS and checks the response
 * register after each send, for up to RTSSMB_S3_ACK_TIMEOUT_MS total.
 *
 * Return: 0 on success, -ETIMEDOUT if RTSS does not acknowledge in time.
 */
static int rtssmb_suspend_handshake(void)
{
	const int retries = RTSSMB_S3_ACK_TIMEOUT_MS / RTSSMB_S3_RETRY_INTERVAL_MS;
	int i;

	dev_dbg(rtssmb_ctx->dev, "sending s3\n");
	rtssmb_send_handshake(rtssmb_ctx->req_tcsr_regmap, rtssmb_ctx->req_tcsr_offset,
			rtssmb_ctx->match_data->handshake_magic[RTSSMB_HANDSHAKE_S3]);

	for (i = 0; i < retries; i++) {

		if ((i << 1) == retries) {
			dev_dbg(rtssmb_ctx->dev, "retry sending s3\n");
			rtssmb_send_handshake(rtssmb_ctx->req_tcsr_regmap, rtssmb_ctx->req_tcsr_offset,
					rtssmb_ctx->match_data->handshake_magic[RTSSMB_HANDSHAKE_S3]);
		}

		msleep(RTSSMB_S3_RETRY_INTERVAL_MS);

		if (rtssmb_read_tcsr_magic(rtssmb_ctx->resp_tcsr_regmap, rtssmb_ctx->resp_tcsr_offset) ==
				rtssmb_ctx->match_data->handshake_magic[RTSSMB_HANDSHAKE_S3]) {
			dev_dbg(rtssmb_ctx->dev, "S3 ack received after %d retries\n", i);
			return 0;
		}

	}

	return -ETIMEDOUT;
}

/**
 * rtssmb_parse_mem_regions() - Parse mailbox and OTA physical regions from DT.
 * @pdev: Platform device.
 * @ctx:  Driver context to populate.
 *
 * Mailbox: memory-region[0] = sail_mailbox_mem. mb_size is always the
 *   region's native resource_size() — the allocated carveout limit — and is
 *   never widened or shrunk by DT. ctx->match_data->handle_offset/handle_size
 *   gives the actual RTSS descriptor's offset from the region base and its
 *   size directly: mb_addr + offset is the actual MB address, size is its
 *   actual size. Both are validated to stay within [0, mb_size]
 *   (offset + size <= mb_size) so bad platform data can't make
 *   RTSS_MB_GET_MB_REGION describe a window outside the allocated carveout.
 *   handle_size == 0 is a legacy choice: RTSS_MB_GET_MB_REGION falls back to
 *   the derived mb_size - mb_offset (safe from underflow since offset is
 *   already bounds-checked here). KMD maps the full region and returns the
 *   offset to UMD via RTSS_MB_GET_MB_REGION IOCTL so UMD can locate the
 *   descriptor without any hardcoded address.
 *
 * OTA: memory-region[1] = sail_ota_mem. Fixed from the map, no offset/size
 *   override.
 *
 * Return: 0 on success, negative error on DT parse failure.
 */
static int rtssmb_parse_mem_regions(struct platform_device *pdev,
				    struct rtssmb_handle *ctx)
{
	struct device_node *mem_node;
	struct resource mem_res;
	u32 handle_offset;
	u32 handle_size;

	/* mailbox: memory-region[0] = sail_mailbox_mem */
	mem_node = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (!mem_node || of_address_to_resource(mem_node, 0, &mem_res)) {
		of_node_put(mem_node);
		dev_err(&pdev->dev, "mailbox memory-region not found\n");
		return -EINVAL;
	}
	ctx->mb_addr = mem_res.start;
	ctx->mb_size = resource_size(&mem_res);
	of_node_put(mem_node);

	handle_offset = ctx->match_data->handle_offset;
	handle_size   = ctx->match_data->handle_size;

	if (handle_offset > ctx->mb_size ||
	    handle_size > ctx->mb_size - handle_offset) {
		dev_err(&pdev->dev,
			"match data handle offset out of range (offset=0x%x size=0x%x mb_size=0x%zx)\n",
			handle_offset, handle_size, ctx->mb_size);
		return -EINVAL;
	}

	ctx->mb_offset = handle_offset;
	ctx->mb_handle_size = handle_size;

	dev_dbg(&pdev->dev, "mb: addr=0x%llx size=0x%zx desc_offset=0x%x handle_size=0x%x\n",
		(unsigned long long)ctx->mb_addr,
		ctx->mb_size, ctx->mb_offset, ctx->mb_handle_size);

	/* OTA: memory-region[1] = sail_ota_mem, no descriptor offset */
	mem_node = of_parse_phandle(pdev->dev.of_node, "memory-region", 1);
	if (!mem_node || of_address_to_resource(mem_node, 0, &mem_res)) {
		of_node_put(mem_node);
		dev_err(&pdev->dev, "OTA memory-region not found\n");
		return -EINVAL;
	}
	ctx->ota_addr   = mem_res.start;
	ctx->ota_size   = resource_size(&mem_res);
	ctx->ota_offset = 0;
	of_node_put(mem_node);
	dev_dbg(&pdev->dev, "ota: addr=0x%llx size=0x%zx\n",
		(unsigned long long)ctx->ota_addr, ctx->ota_size);

	return 0;
}

/**
 * rtssmb_populate_irq()
 *
 * @pdev:       Platform device.
 * @rtssmb_ctx: Driver context to populate irqinfo array.
 *
 * Reads client_id and signal_id from the DT "interrupts-extended" property,
 * resolves each entry's phandle to its target ipccN node to populate sender,
 * requests each IRQ with rtssmb_intr_handler, and leaves them disabled until
 * userspace calls RTSS_MB_SET_EVENT_FD.
 *
 * Return: 0 on success, negative error on DT parse or IRQ request failure.
 */
static int rtssmb_populate_irq(struct platform_device *pdev, struct rtssmb_handle *rtssmb_ctx)
{
	int ret;
	int count = 0;
	int sender;
	struct of_phandle_args args;
	const char *intr_name = "rtssmb_irq";
	const size_t irq_name_size = strlen(intr_name) + 1;

	rtssmb_ctx->irq_count = of_count_phandle_with_args(pdev->dev.of_node,
		"interrupts-extended", "#interrupt-cells");
	if (rtssmb_ctx->irq_count <= 0) {
		dev_err(rtssmb_ctx->dev, "invalid interrupt count %d\n", rtssmb_ctx->irq_count);
		return -EINVAL;
	}

	rtssmb_ctx->irqinfo = devm_kzalloc(&pdev->dev,
		rtssmb_ctx->irq_count * sizeof(*rtssmb_ctx->irqinfo), GFP_KERNEL);
	if (!rtssmb_ctx->irqinfo)
		return -ENOMEM;

	rtssmb_ctx->irq_snapshot = devm_kcalloc(&pdev->dev,
		rtssmb_ctx->irq_count, sizeof(*rtssmb_ctx->irq_snapshot), GFP_KERNEL);
	if (!rtssmb_ctx->irq_snapshot)
		return -ENOMEM;

	for (count = 0; count < rtssmb_ctx->irq_count; count++) {
		ret = of_parse_phandle_with_args(pdev->dev.of_node, "interrupts-extended",
			"#interrupt-cells", count, &args);
		if (ret) {
			dev_err(rtssmb_ctx->dev,
				"failed to parse interrupts-extended entry %d, err=%d\n", count, ret);
			return ret;
		}

		if (args.args_count != 3) {
			dev_err(rtssmb_ctx->dev,
				"interrupts-extended entry %d has %d args, expected 3\n",
				count, args.args_count);
			of_node_put(args.np);
			return -EINVAL;
		}

		sender = rtssmb_sender_for_phandle(pdev, args.np);
		of_node_put(args.np);
		if (sender < 0)
			return sender;

		rtssmb_ctx->irqinfo[count].client_id = args.args[0];
		rtssmb_ctx->irqinfo[count].signal_id = args.args[1];
		rtssmb_ctx->irqinfo[count].sender    = (unsigned int)sender;

		ret = platform_get_irq(pdev, count);
		if (ret < 0) {
			dev_err(rtssmb_ctx->dev, "failed to get irq %d: %d\n", count, ret);
			return ret;
		}
		rtssmb_ctx->irqinfo[count].irq = (unsigned int)ret;
		rtssmb_ctx->irqinfo[count].irq_name =
				devm_kzalloc(&pdev->dev, irq_name_size, GFP_KERNEL);
		if (!rtssmb_ctx->irqinfo[count].irq_name)
			return -ENOMEM;
		strscpy(rtssmb_ctx->irqinfo[count].irq_name, intr_name, irq_name_size);
		INIT_WORK(&rtssmb_ctx->irqinfo[count].signal_work, rtssmb_signal_work);

		dev_dbg(rtssmb_ctx->dev, "requesting irq %d\n", rtssmb_ctx->irqinfo[count].irq);
		ret = devm_request_irq(&pdev->dev, rtssmb_ctx->irqinfo[count].irq,
			rtssmb_intr_handler, 0,
			rtssmb_ctx->irqinfo[count].irq_name, rtssmb_ctx);
		if (ret) {
			dev_err(rtssmb_ctx->dev, "irq %d request failed: %d\n",
				 rtssmb_ctx->irqinfo[count].irq, ret);
			return ret;
		}

		disable_irq_nosync(rtssmb_ctx->irqinfo[count].irq);
	}

	dev_dbg(rtssmb_ctx->dev, "irq init ok\n");
	return 0;
}

/**
 * rtssmb_populate_mbox() - Parse DT mboxes property and request mailbox channels.
 * @pdev:       Platform device.
 * @rtssmb_ctx: Driver context to populate chaninfo array.
 *
 * Reads client_id and signal_id from the DT "mboxes" property, resolves each
 * entry's phandle to its target ipccN node to populate sender, and calls
 * mbox_request_channel() for each TX channel.
 *
 * Return: 0 on success, negative error on DT parse or channel request failure.
 */
static int rtssmb_populate_mbox(struct platform_device *pdev, struct rtssmb_handle *rtssmb_ctx)
{
	int ret;
	int count = 0;
	int sender;
	struct of_phandle_args args;

	rtssmb_ctx->mbox_count = of_count_phandle_with_args(pdev->dev.of_node, "mboxes",
			"#mbox-cells");
	if (rtssmb_ctx->mbox_count <= 0) {
		dev_err(rtssmb_ctx->dev, "invalid mbox count %d\n", rtssmb_ctx->mbox_count);
		return -EINVAL;
	}

	rtssmb_ctx->chaninfo = devm_kzalloc(&pdev->dev,
		rtssmb_ctx->mbox_count * sizeof(*rtssmb_ctx->chaninfo), GFP_KERNEL);
	if (!rtssmb_ctx->chaninfo)
		return -ENOMEM;

	for (count = 0; count < rtssmb_ctx->mbox_count; count++) {
		ret = of_parse_phandle_with_args(pdev->dev.of_node, "mboxes",
			"#mbox-cells", count, &args);
		if (ret) {
			dev_err(rtssmb_ctx->dev,
				"failed to parse mboxes entry %d, err=%d\n", count, ret);
			return ret;
		}

		if (args.args_count != 2) {
			dev_err(rtssmb_ctx->dev,
				"mboxes entry %d has %d args, expected 2\n", count, args.args_count);
			of_node_put(args.np);
			return -EINVAL;
		}

		sender = rtssmb_sender_for_phandle(pdev, args.np);
		of_node_put(args.np);
		if (sender < 0)
			return sender;

		rtssmb_ctx->chaninfo[count].client_id = args.args[0];
		rtssmb_ctx->chaninfo[count].signal_id = args.args[1];
		rtssmb_ctx->chaninfo[count].sender    = (unsigned int)sender;

		dev_dbg(&pdev->dev, "requesting mbox %d\n", count);
		rtssmb_ctx->chaninfo[count].client.dev = &pdev->dev;
		rtssmb_ctx->chaninfo[count].client.knows_txdone = true;

		rtssmb_ctx->chaninfo[count].mchan =
			mbox_request_channel(&rtssmb_ctx->chaninfo[count].client,
				count);
		if (IS_ERR(rtssmb_ctx->chaninfo[count].mchan)) {
			dev_err(rtssmb_ctx->dev, "failed to get ipc mailbox %ld\n",
				PTR_ERR(rtssmb_ctx->chaninfo[count].mchan));
			ret = PTR_ERR(rtssmb_ctx->chaninfo[count].mchan);
			return ret;
		}
	}

	dev_info(rtssmb_ctx->dev, "mbox init ok\n");
	return 0;
}

/**
 * rtssmb_probe() - Platform driver probe: initialise /dev/rtssmb and handshake RTSS.
 * @pdev: Platform device matched by "qcom,rtss-mailbox" compatible string.
 *
 * Maps TCSR and IPC registers, creates the /dev/rtssmb character device,
 * populates mailbox channels and IRQs from DT, parses memory-region physical
 * addresses, and performs the S1/S2 boot handshake with RTSS.
 *
 * Return: 0 on success, negative error on any initialisation failure.
 */
static int rtssmb_probe(struct platform_device *pdev)
{
	int ret;
	struct device *dev;

	rtssmb_ctx = devm_kzalloc(&pdev->dev, sizeof(*rtssmb_ctx), GFP_KERNEL);
	if (!rtssmb_ctx)
		return -ENOMEM;

	rtssmb_ctx->dev = &pdev->dev;

	rtssmb_ctx->match_data = of_device_get_match_data(&pdev->dev);
	if (!rtssmb_ctx->match_data) {
		dev_err(rtssmb_ctx->dev, "no match data for this compatible string\n");
		return -ENODEV;
	}

	rtssmb_ctx->req_tcsr_regmap = syscon_regmap_lookup_by_phandle_args(pdev->dev.of_node,
			"qcom,syscon-tcsr-req", 1, &rtssmb_ctx->req_tcsr_offset);
	if (IS_ERR(rtssmb_ctx->req_tcsr_regmap)) {
		dev_err(rtssmb_ctx->dev, "qcom,syscon-tcsr-req regmap lookup failed\n");
		return PTR_ERR(rtssmb_ctx->req_tcsr_regmap);
	}

	rtssmb_ctx->resp_tcsr_regmap = syscon_regmap_lookup_by_phandle_args(pdev->dev.of_node,
			"qcom,syscon-tcsr-resp", 1, &rtssmb_ctx->resp_tcsr_offset);
	if (IS_ERR(rtssmb_ctx->resp_tcsr_regmap)) {
		dev_err(rtssmb_ctx->dev, "qcom,syscon-tcsr-resp regmap lookup failed\n");
		return PTR_ERR(rtssmb_ctx->resp_tcsr_regmap);
	}

	rtssmb_ctx->ipc_regmap = syscon_regmap_lookup_by_phandle_args(pdev->dev.of_node,
			"qcom,syscon-ipc", 1, &rtssmb_ctx->ipc_offset);
	if (IS_ERR(rtssmb_ctx->ipc_regmap)) {
		dev_err(rtssmb_ctx->dev, "qcom,syscon-ipc regmap lookup failed\n");
		return PTR_ERR(rtssmb_ctx->ipc_regmap);
	}

	rtssmb_ctx->tcsr_word_packed = rtssmb_ctx->match_data->tcsr_word_packed;

	mutex_init(&rtssmb_ctx->dev_lock);
	spin_lock_init(&rtssmb_ctx->irq_lock);

	ret = alloc_chrdev_region(&rtssmb_ctx->rtssmb_devnum, 0, 1, "rtssmb");
	if (ret) {
		dev_err(rtssmb_ctx->dev, "chrdev alloc failed: %d\n", ret);
		goto failed_chrdev_alloc;
	}

	cdev_init(&rtssmb_ctx->rtssmb_cdev, &rtssmb_fops);
	ret = cdev_add(&rtssmb_ctx->rtssmb_cdev, rtssmb_ctx->rtssmb_devnum, 1);
	if (ret) {
		dev_err(rtssmb_ctx->dev, "cdev add failed: %d\n", ret);
		goto failed_cdev_add;
	}

	rtssmb_ctx->rtssmb_devmaj = MAJOR(rtssmb_ctx->rtssmb_devnum);

	rtssmb_ctx->rtssmb_class = rtssmb_class_create("rtssmb");
	if (IS_ERR(rtssmb_ctx->rtssmb_class)) {
		ret = PTR_ERR(rtssmb_ctx->rtssmb_class);
		dev_err(rtssmb_ctx->dev, "class create failed: %d\n", ret);
		goto failed_class_create;
	}

	dev = device_create(rtssmb_ctx->rtssmb_class, NULL,
			rtssmb_ctx->rtssmb_devnum, NULL, "rtssmb");
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		dev_err(rtssmb_ctx->dev, "device create failed: %d\n", ret);
		goto failed_device_create;
	}

	rtssmb_ctx->sender_lut = rtssmb_ctx->match_data->sender_lut;
	rtssmb_ctx->sender_lut_count = rtssmb_ctx->match_data->sender_lut_count;

	ret = rtssmb_populate_mbox(pdev, rtssmb_ctx);
	if (ret)
		goto out;

	ret = rtssmb_populate_irq(pdev, rtssmb_ctx);
	if (ret)
		goto out;

	ret = rtssmb_parse_mem_regions(pdev, rtssmb_ctx);
	if (ret)
		goto out;

	rtssmb_ctx->rtss_handshake_delay = rtssmb_ctx->match_data->handshake_delay_us;
	if (rtssmb_ctx->rtss_handshake_delay < 100)
		rtssmb_ctx->rtss_handshake_delay = 100;

	ret = rtssmb_init_handshakes();
	if (ret) {
		dev_err(rtssmb_ctx->dev, "handshake failed: %d\n", ret);
		goto out;
	}

	dev_info(rtssmb_ctx->dev, "probe success\n");
	return 0;

out:
	device_destroy(rtssmb_ctx->rtssmb_class, rtssmb_ctx->rtssmb_devnum);
failed_device_create:
	class_destroy(rtssmb_ctx->rtssmb_class);
failed_class_create:
	cdev_del(&rtssmb_ctx->rtssmb_cdev);
failed_cdev_add:
	unregister_chrdev_region(rtssmb_ctx->rtssmb_devnum, 1);
failed_chrdev_alloc:
	return ret;
}

/**
 * rtssmb_remove() - Platform driver remove: tear down /dev/rtssmb.
 * @pdev: Platform device.
 *
 * Return: void on kernel >= 6.11, 0 on older kernels.
 */
static RTSSMB_REMOVE_RETURN_TYPE rtssmb_remove(struct platform_device *pdev)
{
	device_destroy(rtssmb_ctx->rtssmb_class, rtssmb_ctx->rtssmb_devnum);
	class_destroy(rtssmb_ctx->rtssmb_class);
	cdev_del(&rtssmb_ctx->rtssmb_cdev);
	unregister_chrdev_region(rtssmb_ctx->rtssmb_devnum, 1);
	RTSSMB_REMOVE_RETURN;
}

/**
 * rtssmb_suspend() - PM suspend: notify RTSS and wait for ACK.
 * @dev: Device.
 *
 * Disables all active RX IRQs before sending S3 to RTSS so no interrupt
 * fires after RTSS goes down. On handshake failure, re-enables IRQs and
 * retries S1/S2 to restore communication.
 *
 * Return: 0 on success, -ETIMEDOUT if RTSS does not acknowledge.
 */
static int rtssmb_suspend(struct device *dev)
{
	unsigned long flags;
	int active_count = 0;
	int ret;
	int i;

	/* Snapshot active IRQ numbers under lock — disable_irq_nosync must not
	 * be called while holding a spinlock (IRQ chip callbacks may deadlock).
	 * irq_snapshot[] is pre-allocated at probe, so no alloc needed here.
	 */
	spin_lock_irqsave(&rtssmb_ctx->irq_lock, flags);
	for (i = 0; i < rtssmb_ctx->irq_count; i++) {
		if (rtssmb_ctx->irqinfo[i].event_fd_sig)
			rtssmb_ctx->irq_snapshot[active_count++] = rtssmb_ctx->irqinfo[i].irq;
	}
	spin_unlock_irqrestore(&rtssmb_ctx->irq_lock, flags);

	for (i = 0; i < active_count; i++)
		disable_irq_nosync(rtssmb_ctx->irq_snapshot[i]);

	/* Synchronize to ensure any in-flight IRQ handlers complete */
	for (i = 0; i < active_count; i++)
		synchronize_irq(rtssmb_ctx->irq_snapshot[i]);

	ret = rtssmb_suspend_handshake();
	if (ret) {
		dev_err(rtssmb_ctx->dev, "suspend failed: %d\n", ret);
		for (i = 0; i < active_count; i++)
			enable_irq(rtssmb_ctx->irq_snapshot[i]);
		ret = rtssmb_init_handshakes();
		if (ret)
			dev_err(rtssmb_ctx->dev, "suspend recovery handshake failed: %d\n", ret);
		return -ETIMEDOUT;
	}

	return 0;
}

/**
 * rtssmb_resume() - PM resume: re-run S1/S2 handshake and re-enable RX IRQs.
 * @dev: Device.
 *
 * Re-establishes communication with RTSS then re-enables all IRQs that had
 * active eventfds at suspend time so RX channels resume receiving.
 *
 * Return: 0 on success, negative error if handshake fails.
 */
static int rtssmb_resume(struct device *dev)
{
	unsigned long flags;
	int active_count = 0;
	int ret;
	int i;

	dev_dbg(rtssmb_ctx->dev, "resuming\n");

	ret = rtssmb_init_handshakes();
	if (ret) {
		dev_err(rtssmb_ctx->dev, "resume handshake failed: %d\n", ret);
		return ret;
	}

	/* Re-enable IRQs for channels that were active before suspend.
	 * Snapshot under lock, call enable_irq outside the lock.
	 */
	spin_lock_irqsave(&rtssmb_ctx->irq_lock, flags);
	for (i = 0; i < rtssmb_ctx->irq_count; i++) {
		if (rtssmb_ctx->irqinfo[i].event_fd_sig)
			rtssmb_ctx->irq_snapshot[active_count++] = rtssmb_ctx->irqinfo[i].irq;
	}
	spin_unlock_irqrestore(&rtssmb_ctx->irq_lock, flags);

	for (i = 0; i < active_count; i++)
		enable_irq(rtssmb_ctx->irq_snapshot[i]);

	return 0;
}

static const struct dev_pm_ops rtssmb_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(rtssmb_suspend, rtssmb_resume)
};

static const struct rtssmb_sender_lut_entry rtssmb_sa8775p_sender_lut[] = {
	{ 0x488000, 0x8 },
	{ 0x4a1000, 0x21 },
	{ 0x4a2000, 0x22 },
	{ 0x4a3000, 0x23 },
};

static const u32 rtssmb_sa8775p_handshake_magic[RTSSMB_HANDSHAKE_MAGIC_COUNT] = {
	0xD0000000, 0xD0020000, 0xD0030000,
};

static const struct rtssmb_match_data rtssmb_sa8775p_data = {
	.handshake_magic     = rtssmb_sa8775p_handshake_magic,
	.handshake_delay_us  = RTSSMB_IPC_DEFAULT_DLY_US,
	.handle_offset       = 0x80000,
	.handle_size         = 0x80000,
	.sender_lut          = rtssmb_sa8775p_sender_lut,
	.sender_lut_count    = ARRAY_SIZE(rtssmb_sa8775p_sender_lut),
	.tcsr_word_packed    = false,
};

static const struct rtssmb_sender_lut_entry rtssmb_qcs8300_sender_lut[] = {
	{ 0x488000, 0x8 },
	{ 0x4a1000, 0x21 },
	{ 0x4a2000, 0x22 },
	{ 0x4a3000, 0x23 },
};

static const u32 rtssmb_qcs8300_handshake_magic[RTSSMB_HANDSHAKE_MAGIC_COUNT] = {
	0xD0000000, 0xD0020000, 0xD0030000,
};

static const struct rtssmb_match_data rtssmb_qcs8300_data = {
	.handshake_magic     = rtssmb_qcs8300_handshake_magic,
	.handshake_delay_us  = RTSSMB_IPC_DEFAULT_DLY_US,
	.handle_offset       = 0x80000,
	.handle_size         = 0x80000,
	.sender_lut          = rtssmb_qcs8300_sender_lut,
	.sender_lut_count    = ARRAY_SIZE(rtssmb_qcs8300_sender_lut),
	.tcsr_word_packed    = false,
};

static const struct of_device_id rtssmb_dt_match[] = {
	{ .compatible = "qcom,sa8775p-rtss-mailbox", .data = &rtssmb_sa8775p_data },
	{ .compatible = "qcom,qcs8300-rtss-mailbox", .data = &rtssmb_qcs8300_data },
	/*
	 * Add a new qcom,<chipset>-rtss-mailbox entry + rtssmb_match_data
	 * block above for each future platform.
	 */
	{}
};

MODULE_DEVICE_TABLE(of, rtssmb_dt_match);

static struct platform_driver rtssmb_driver = {
	.driver = {
		.name = "rtss_mailbox",
		.of_match_table = rtssmb_dt_match,
		.suppress_bind_attrs = true,
		.pm = pm_ptr(&rtssmb_pm_ops),
	},
	.probe = rtssmb_probe,
	.remove = rtssmb_remove,
};

module_platform_driver(rtssmb_driver);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. rtss mailbox driver");
MODULE_LICENSE("GPL v2");
