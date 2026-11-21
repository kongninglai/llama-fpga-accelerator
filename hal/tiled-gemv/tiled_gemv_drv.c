// SPDX-License-Identifier: GPL-2.0
/* Linux 5.15, Ultra96. One exclusive client and one active DMA job. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/kref.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/compat.h>
#include <linux/clk.h>
#include "tiled_gemv_abi.h"

static unsigned int arena_mb = 640;
module_param(arena_mb, uint, 0444);
MODULE_PARM_DESC(arena_mb, "Requested DMA weight arena MiB; may shrink on allocation failure");
static unsigned int engines = 2;
module_param(engines, uint, 0444);
MODULE_PARM_DESC(engines, "Number of connected memory engines, 1 or 2");

struct tg_buffer {
    void *cpu;
    dma_addr_t dma;
    size_t bytes;
};

struct tg_dev {
    struct device *dev;
    void __iomem *regs;
    struct miscdevice misc;
    struct mutex lock;
    struct kref refs;
    struct tg_buffer buffer[3];
    struct tg_info info;
    struct tg_completion completion;
    bool opened, removed, active, poisoned;
};

static void tg_free(struct kref *ref)
{
    struct tg_dev *d = container_of(ref, struct tg_dev, refs);
    unsigned i;
    /* Closing or a timeout is not a DMA cancellation. Keep allocations if the
     * device disappeared before completion; otherwise hardware could overwrite
     * memory reused by Linux. A board reset is required to reclaim this case.
     */
    if (d->active) {
        dev_err(d->dev, "DMA completion unknown; retaining buffers until reboot\n");
    } else {
        for (i = 0; i < 3; ++i)
            if (d->buffer[i].cpu)
                dma_free_coherent(d->dev, d->buffer[i].bytes,
                                  d->buffer[i].cpu, d->buffer[i].dma);
    }
    put_device(d->dev);
    kfree(d);
}

static void tg_disable_irqs(struct tg_dev *d)
{
    u32 pending;
    writel(0, d->regs + TG_REG_GIE);
    writel(0, d->regs + TG_REG_IER);
    pending = readl(d->regs + TG_REG_ISR) & 3;
    if (pending) writel(pending, d->regs + TG_REG_ISR);
}

static void tg_refresh(struct tg_dev *d)
{
    if (d->active && (readl(d->regs + TG_REG_CTRL) & TG_CTRL_DONE)) {
        dma_rmb();
        d->completion.hardware_status = readl(d->regs + TG_REG_STATUS);
        d->completion.state = d->completion.hardware_status == TG_STATUS_DONE ?
                              TG_JOB_DONE : TG_JOB_ERROR;
        if (d->completion.state == TG_JOB_ERROR) d->poisoned = true;
        d->active = false;
        d->info.completed++;
    }
}

/* Called under lock; no hardware access is allowed once removed is set. */
static int tg_wait_locked(struct tg_dev *d, unsigned int ms)
{
    u64 deadline = ktime_get_ns() + (u64)ms * NSEC_PER_MSEC;
    unsigned polls = 0;
    for (;;) {
        tg_refresh(d);
        if (!d->active) return 0;
        if (ktime_get_ns() >= deadline) return -ETIMEDOUT;
        cpu_relax();
        if (!(++polls & 1023)) cond_resched();
    }
}

static int tg_open(struct inode *inode, struct file *file)
{
    struct tg_dev *d = container_of(file->private_data, struct tg_dev, misc);
    int rc = 0;
    mutex_lock(&d->lock);
    if (d->removed) rc = -ENODEV;
    else if (d->opened) rc = -EBUSY;
    else {
        tg_refresh(d);
        if (d->active || d->poisoned) rc = -EIO;
        else {
            d->opened = true;
            memset(&d->completion, 0, sizeof(d->completion));
            kref_get(&d->refs);
            file->private_data = d;
        }
    }
    mutex_unlock(&d->lock);
    return rc;
}

static int tg_release(struct inode *inode, struct file *file)
{
    struct tg_dev *d = file->private_data;
    mutex_lock(&d->lock);
    if (!d->removed && d->active && tg_wait_locked(d, 1000)) {
        d->poisoned = true;
        dev_err(d->dev, "client closed during DMA; buffers quarantined\n");
    }
    d->opened = false;
    mutex_unlock(&d->lock);
    kref_put(&d->refs, tg_free);
    return 0;
}

static void tg_wr64(struct tg_dev *d, unsigned reg, dma_addr_t addr)
{
    writel(lower_32_bits(addr), d->regs + reg);
    writel(upper_32_bits(addr), d->regs + reg + 4);
}

static int tg_submit(struct tg_dev *d, const struct tg_job *j)
{
    u32 first;
    u64 row;
    dma_addr_t w, y, out;
    if (!tg_job_valid(j, &d->info)) return -EINVAL;
    if (d->poisoned) return -EIO;
    tg_refresh(d);
    if (d->poisoned) return -EIO;
    if (d->active) return -EBUSY;
    if (j->sequence <= d->completion.sequence) return -ESTALE;
    if (!(readl(d->regs + TG_REG_CTRL) & TG_CTRL_IDLE)) return -EBUSY;
    first = tg_split_rows(j->rows, d->info.engines);
    row = (u64)j->nb * TG_BLOCK_BYTES;
    w = d->buffer[0].dma + j->weight_offset;
    y = d->buffer[1].dma + j->y_offset;
    out = d->buffer[2].dma + j->out_offset;
    tg_disable_irqs(d);
    tg_wr64(d, TG_REG_W0, w);
    tg_wr64(d, TG_REG_W1, w + first * row);
    tg_wr64(d, TG_REG_Y0, y);
    tg_wr64(d, TG_REG_Y1, y);
    tg_wr64(d, TG_REG_OUT0, out);
    tg_wr64(d, TG_REG_OUT1, out + first * sizeof(float));
    writel(j->nb, d->regs + TG_REG_NB);
    writel(first, d->regs + TG_REG_ROWS0);
    writel(j->rows - first, d->regs + TG_REG_ROWS1);
    memset(&d->completion, 0, sizeof(d->completion));
    d->completion.sequence = j->sequence;
    d->completion.state = TG_JOB_BUSY;
    d->active = true;
    d->info.submitted++;
    /* Publish userspace writes to coherent mappings before the doorbell. */
    dma_wmb();
    writel(TG_CTRL_START, d->regs + TG_REG_CTRL);
    readl(d->regs + TG_REG_GIE); /* flush posted write without clearing ap_done */
    return 0;
}

static long tg_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct tg_dev *d = file->private_data;
    void __user *ptr = (void __user *)arg;
    struct tg_job job;
    struct tg_completion c;
    int rc = 0;
    if (cmd == TG_IOC_SUBMIT && copy_from_user(&job, ptr, sizeof(job))) return -EFAULT;
    if ((cmd == TG_IOC_QUERY || cmd == TG_IOC_WAIT) && copy_from_user(&c, ptr, sizeof(c)))
        return -EFAULT;
    mutex_lock(&d->lock);
    if (d->removed) { rc = -ENODEV; goto out; }
    switch (cmd) {
    case TG_IOC_INFO:
        tg_refresh(d);
        if (copy_to_user(ptr, &d->info, sizeof(d->info))) rc = -EFAULT;
        break;
    case TG_IOC_SUBMIT:
        rc = tg_submit(d, &job);
        break;
    case TG_IOC_QUERY:
    case TG_IOC_WAIT:
        if (!c.sequence || c.sequence != d->completion.sequence) { rc = -ESTALE; break; }
        if (c.reserved || c.timeout_ms > 60000) { rc = -EINVAL; break; }
        if (cmd == TG_IOC_WAIT) rc = tg_wait_locked(d, c.timeout_ms ? c.timeout_ms : 20000);
        else tg_refresh(d);
        c.state = d->completion.state;
        c.hardware_status = d->completion.hardware_status;
        c.reserved = 0;
        if (copy_to_user(ptr, &c, sizeof(c))) rc = -EFAULT;
        break;
    default:
        rc = -ENOTTY;
    }
out:
    mutex_unlock(&d->lock);
    return rc;
}

static int tg_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct tg_dev *d = file->private_data;
    unsigned long region = vma->vm_pgoff;
    int rc;
    mutex_lock(&d->lock);
    if (d->removed) rc = -ENODEV;
    else if (region >= 3 || !(vma->vm_flags & VM_SHARED) ||
             vma->vm_end - vma->vm_start != d->buffer[region].bytes) rc = -EINVAL;
    else {
        vma->vm_pgoff = 0;
        vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
        rc = dma_mmap_coherent(d->dev, vma, d->buffer[region].cpu,
                              d->buffer[region].dma, d->buffer[region].bytes);
        /* vm_file keeps the exclusive file and its device reference alive
         * until the last mapping closes, including inherited mappings. */
    }
    mutex_unlock(&d->lock);
    return rc;
}

static const struct file_operations tg_fops = {
    .owner = THIS_MODULE,
    .open = tg_open,
    .release = tg_release,
    .unlocked_ioctl = tg_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = compat_ptr_ioctl,
#endif
    .mmap = tg_mmap,
    .llseek = no_llseek,
};

static int tg_alloc(struct tg_dev *d, unsigned region, size_t bytes)
{
    struct tg_buffer *b = &d->buffer[region];
    b->bytes = PAGE_ALIGN(bytes);
    b->cpu = dma_alloc_coherent(d->dev, b->bytes, &b->dma, GFP_KERNEL | __GFP_NOWARN);
    return b->cpu ? 0 : -ENOMEM;
}

static void tg_clk_disable(void *clock)
{
    clk_disable_unprepare(clock);
}

static int tg_probe(struct platform_device *pdev)
{
    struct tg_dev *d;
    struct clk *clock;
    struct resource *res;
    unsigned mb;
    int rc;
    if ((engines != 1 && engines != 2) || arena_mb < 16 || arena_mb > 1024) return -EINVAL;
    d = kzalloc(sizeof(*d), GFP_KERNEL);
    if (!d) return -ENOMEM;
    d->dev = get_device(&pdev->dev);
    mutex_init(&d->lock);
    kref_init(&d->refs);
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!res || resource_size(res) < TG_REG_STATUS + 8) { rc = -EINVAL; goto fail; }
    d->regs = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(d->regs)) { rc = PTR_ERR(d->regs); goto fail; }
    clock = devm_clk_get_optional(&pdev->dev, "ap_clk");
    if (IS_ERR(clock)) { rc = PTR_ERR(clock); goto fail; }
    rc = clk_prepare_enable(clock);
    if (rc) goto fail;
    rc = devm_add_action_or_reset(&pdev->dev, tg_clk_disable, clock);
    if (rc) goto fail;
    /* Do not claim a running IP or replace its DMA addresses. */
    if (!(readl(d->regs + TG_REG_CTRL) & TG_CTRL_IDLE)) { rc = -EBUSY; goto fail; }
    rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
    if (rc) goto fail;
    rc = tg_alloc(d, 1, 64 * 1024);
    if (rc) goto fail;
    rc = tg_alloc(d, 2, TG_MAX_ROWS * sizeof(float));
    if (rc) goto fail;
    for (mb = arena_mb; mb >= 16; mb /= 2) {
        rc = tg_alloc(d, 0, (size_t)mb << 20);
        if (!rc) break;
    }
    if (rc) goto fail;
    d->info.arena_bytes = d->buffer[0].bytes;
    d->info.y_bytes = d->buffer[1].bytes;
    d->info.out_bytes = d->buffer[2].bytes;
    d->info.version = TG_ABI_VERSION;
    d->info.engines = engines;
    tg_disable_irqs(d);
    d->misc.minor = MISC_DYNAMIC_MINOR;
    d->misc.name = "tiled_gemv";
    d->misc.fops = &tg_fops;
    d->misc.parent = &pdev->dev;
    d->misc.mode = 0600;
    rc = misc_register(&d->misc);
    if (rc) goto fail;
    platform_set_drvdata(pdev, d);
    dev_info(&pdev->dev, "%u engines, %zu MiB arena (requested %u), clock %lu Hz\n",
             engines, d->buffer[0].bytes >> 20, arena_mb, clk_get_rate(clock));
    return 0;
fail:
    kref_put(&d->refs, tg_free);
    return rc;
}

static int tg_remove(struct platform_device *pdev)
{
    struct tg_dev *d = platform_get_drvdata(pdev);
    misc_deregister(&d->misc);
    mutex_lock(&d->lock);
    if (d->active) tg_wait_locked(d, 1000);
    tg_disable_irqs(d);
    d->removed = true;
    mutex_unlock(&d->lock);
    /* Existing mappings retain DMA allocations until their files close. */
    kref_put(&d->refs, tg_free);
    return 0;
}

static const struct of_device_id tg_of_match[] = {
    { .compatible = "ece382m,tiled-gemv-1.0" },
    { }
};
MODULE_DEVICE_TABLE(of, tg_of_match);
static struct platform_driver tg_driver = {
    .probe = tg_probe,
    .remove = tg_remove,
    .driver = {
        .name = "tiled_gemv",
        .of_match_table = tg_of_match,
        .suppress_bind_attrs = true,
    },
};
module_platform_driver(tg_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Asynchronous dual-engine Q8_0 tiled GEMV driver");
