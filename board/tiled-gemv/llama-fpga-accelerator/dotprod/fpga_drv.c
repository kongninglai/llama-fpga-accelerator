/*
 *  fpga_drv.c
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/version.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/vmalloc.h>
#include <linux/mman.h>
#include <linux/ioport.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>

#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>

/* HLS board driver. Native AArch64 userspace ABI matches dotProd_app and
 * fpga_dot_hal.h. Hardware offsets come directly from the exported HLS IP.
 * See README.md for the required XSA address-segment correction.
 */
#include "../hls/impl/xdotprod_accel_hw.h"

#ifndef CONFIG_OF  // If no device-tree support in kernel, turn it off here
#define NO_DTS
#endif

#define fpga_VERSION "1.0"
#define fpga_NAME    "fpga_drv"

/* Without device-tree support, compile in hardcoded resource info */
#ifdef NO_DTS
/* Linux IRQ numbers are assigned at boot; do not hard-code the old IRQ 54.
 * For NO_DTS, pass irq_number=<Linux IRQ for the board device tree> at insmod.
 */
static int irq_number = -1;
module_param(irq_number, int, 0444);
#define FPGA_BASE    0xA0000000    
#endif

#define MAP_BYTES           0x40000
#define REG_CONTROL_OFFSET XDOTPROD_ACCEL_CTRL_ADDR_AP_CTRL
#define REG_GIE_OFFSET     XDOTPROD_ACCEL_CTRL_ADDR_GIE
#define REG_IER_OFFSET     XDOTPROD_ACCEL_CTRL_ADDR_IER
#define REG_ISR_OFFSET     XDOTPROD_ACCEL_CTRL_ADDR_ISR
#define REG_LENGTH_OFFSET  XDOTPROD_ACCEL_CTRL_ADDR_N_DATA
#define REG_RESULT_OFFSET  XDOTPROD_ACCEL_CTRL_ADDR_RESULT_DATA
#define REG_STATUS_OFFSET  XDOTPROD_ACCEL_CTRL_ADDR_STATUS_DATA
#define X_MEM_OFFSET       XDOTPROD_ACCEL_CTRL_ADDR_X_BASE
#define Y_MEM_OFFSET       XDOTPROD_ACCEL_CTRL_ADDR_Y_BASE
#define CONTROL_START      BIT(0)
#define CONTROL_IDLE       BIT(2)
#define IRQ_DONE           BIT(0)
#define IRQ_MASK           (BIT(0) | BIT(1))
/* Software/userspace status bits, NOT AP_CTRL bits. */
#define STATUS_BUSY        1U
#define STATUS_DONE        2U
#define STATUS_ERROR       4U
#define NUM_INPUT_BYTES    (1024U * 34U)

#define CMD_START_ACC      (1<<27)
#define CMD_CHECK_STATUS   (2<<27)
#define CMD_RESET_ACC      (3<<27)

/* Commands are matched exactly; arg for START/CHECK is unsigned long *. */

MODULE_AUTHOR("team 17");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FPGA Device Driver");

#define DRIVER_NAME "fpga"

#ifdef NO_DTS
/* command line parameters */
unsigned install = 0;
module_param(install, int, S_IRUGO);
#endif

/* count of received interrupts */
int interruptcount = 0;

/* instance-specific driver-internal data structure */
static struct fpga_drv_local {
  int irq;
  void __iomem *base;
  unsigned long base_start;
  unsigned long base_end;
  struct proc_dir_entry *fpga_interrupt_file;
  struct fasync_struct *fasync_fpga_queue ;
  unsigned long status;
  unsigned long result;
  unsigned long num_elements;

  int device_open;
  bool pending; /* Running or completed but not yet consumed; data_mutex. */

  struct mutex data_mutex;
  spinlock_t state_lock;
} l;

/* Reading AP_CTRL clears ap_done/ap_ready. Completion ownership therefore
 * uses ISR and the saved software status, never AP_CTRL's done bit.
 */
static bool fpga_busy(void)
{
   u32 control = readl(l.base + REG_CONTROL_OFFSET);
   return (control & CONTROL_START) || !(control & CONTROL_IDLE);
}

/* ISR is toggle-on-write: writing 1 to an already-clear bit would SET it.
 * Call under state_lock after IRQ registration; never blindly write 3.
 */
static void fpga_ack_irq(void)
{
   u32 observed = readl(l.base + REG_ISR_OFFSET) & IRQ_MASK;
   if (observed)
      writel(observed, l.base + REG_ISR_OFFSET);
   readl(l.base + REG_ISR_OFFSET); /* Flush the posted acknowledgement. */
}

static void fpga_irq_disable(void *base)
{
   writel(0, base + REG_GIE_OFFSET);
   writel(0, base + REG_IER_OFFSET);
   readl(base + REG_GIE_OFFSET);
}

/* Only aligned 32-bit AXI-Lite writes. Preserve 34-byte block packing;
 * pad just the final half-word for odd block counts, without overreading.
 */
static void fpga_upload(unsigned int offset, const u8 *input, size_t bytes)
{
   size_t i;
   for (i = 0; i < bytes; i += 4) {
      u32 word = input[i];
      unsigned int j;
      for (j = 1; j < 4 && i + j < bytes; ++j)
         word |= (u32)input[i + j] << (8 * j);
      writel(word, l.base + offset + i);
   }
}

/* =========================================================================
 * Interrupt handling
 */

/* Interrupt handler */
static irqreturn_t fpga_int_handler(int irq, void *lp)
{
   u32 observed;

   spin_lock(&l.state_lock);
   observed = readl(l.base + REG_ISR_OFFSET) & IRQ_MASK;
   if (!observed) {
      spin_unlock(&l.state_lock);
      return IRQ_NONE;
   }
   if (observed & IRQ_DONE) {
      l.result = readl(l.base + REG_RESULT_OFFSET);
      l.status = readl(l.base + REG_STATUS_OFFSET);
      /* An unexpected completion payload is a failed request, not a hang. */
      if (l.status != STATUS_DONE)
         l.status = STATUS_DONE | STATUS_ERROR;
      interruptcount++;
   }
   writel(observed, l.base + REG_ISR_OFFSET);
   readl(l.base + REG_ISR_OFFSET);
   spin_unlock(&l.state_lock);

   if (observed & IRQ_DONE)
      kill_fasync(&l.fasync_fpga_queue, SIGIO, POLL_IN);
   return IRQ_HANDLED;
}

/* =========================================================================
 * Driver access methods
 */

static int fpga_open1 (struct inode *inode, struct file *file) {
   unsigned long flags;
   int ret = 0;

   mutex_lock(&l.data_mutex);
   spin_lock_irqsave(&l.state_lock, flags);
   if (l.device_open || (l.pending && !(l.status & STATUS_DONE))) {
      ret = -EBUSY;
   } else {
      l.device_open = 1;
      l.num_elements = 0;
      l.pending = false;
      l.status = 0;
      l.result = 0;
   }
   spin_unlock_irqrestore(&l.state_lock, flags);
   mutex_unlock(&l.data_mutex);
   return ret;
}

static int fpga_release1 (struct inode *inode, struct file *file) {
   mutex_lock(&l.data_mutex);
   fasync_helper(-1, file, 0, &l.fasync_fpga_queue);
   l.device_open = 0;
   /* Preserve pending: closing does not cancel hardware computation. */
   mutex_unlock(&l.data_mutex);
   return 0;
}

static int fpga_fasync1 (int fd, struct file *filp, int on)
{
#ifdef DEBUG
   printk(KERN_INFO "\nfpga_drv: Inside fpga_fasync \n");
#endif
   return fasync_helper(fd, filp, on, &l.fasync_fpga_queue);

} 

/**
 * fpga_write1: process calls this function to write x and y
 * needs to derive element count from the copy count
 */
static ssize_t fpga_write1(struct file *filp, const char __user *buf, size_t count, loff_t *offp)
{
   unsigned char *input;
   size_t num_bytes = count / 2;
   ssize_t ret;

   /* X then Y, each made of complete 34-byte blocks, at most 1024 blocks. */
   if (!count || count % 68 || count > 2 * NUM_INPUT_BYTES)
      return -EINVAL;
   input = memdup_user(buf, count);
   if (IS_ERR(input))
      return PTR_ERR(input);

   mutex_lock(&l.data_mutex);
   if (l.pending || fpga_busy()) {
      ret = -EBUSY;
   } else {
      fpga_upload(X_MEM_OFFSET, input, num_bytes);
      fpga_upload(Y_MEM_OFFSET, input + num_bytes, num_bytes);
      l.num_elements = num_bytes / 34 * 32;
      ret = count;
   }
   mutex_unlock(&l.data_mutex);
   kfree(input);
   return ret;
}

/**
 * fpga_read1: process calls this function to read the result and status
 */
static ssize_t fpga_read1(struct file *filp, char __user *buf, size_t count, loff_t *offp)
{
   /* Exactly 8 bytes on ARM64: float32 result bits followed by status bits. */
   struct { u32 result; u32 status; } output;
   unsigned long flags;
   ssize_t ret;

   if (count < sizeof(output))
      return -EINVAL;
   mutex_lock(&l.data_mutex);
   spin_lock_irqsave(&l.state_lock, flags);
   if (!l.pending || !(l.status & STATUS_DONE)) {
      spin_unlock_irqrestore(&l.state_lock, flags);
      ret = -EAGAIN;
      goto out;
   }
   output.result = l.result;
   output.status = l.status;
   spin_unlock_irqrestore(&l.state_lock, flags);

   /* User copies can sleep, so do not hold the spinlock here. */
   if (copy_to_user(buf, &output, sizeof(output))) {
      ret = -EFAULT;
      goto out; /* Keep completion available for retry. */
   }
   spin_lock_irqsave(&l.state_lock, flags);
   l.status = 0;
   spin_unlock_irqrestore(&l.state_lock, flags);
   l.pending = false;
   ret = sizeof(output);
out:
   mutex_unlock(&l.data_mutex);
   return ret;
}

static long fpga_ioctl1(struct file *file, unsigned int cmd, unsigned long arg){
   int retval = 0;
   unsigned long value, flags;

   mutex_lock(&l.data_mutex);
   switch(cmd)
   {
      case CMD_START_ACC:
         /* Userspace calls ioctl(fd, CMD_START_ACC, &num_elements). */
         if (get_user(value, (unsigned long __user *)arg)) {
            retval = -EFAULT;
            break;
         }
         if (!value || value > 32768 || value % 32 || value != l.num_elements) {
            retval = -EINVAL;
            break;
         }
         if (l.pending || fpga_busy()) {
            retval = -EBUSY;
            break;
         }
         spin_lock_irqsave(&l.state_lock, flags);
         l.status = 0;
         l.result = 0;
         l.pending = true;
         fpga_ack_irq();
         writel(value, l.base + REG_LENGTH_OFFSET);
         writel(CONTROL_START, l.base + REG_CONTROL_OFFSET);
         spin_unlock_irqrestore(&l.state_lock, flags);
         break;

      case CMD_CHECK_STATUS:
         /* Do not overwrite saved DONE/ERROR with acknowledged live status. */
         spin_lock_irqsave(&l.state_lock, flags);
         value = (l.status & STATUS_DONE) ? l.status :
                 (l.pending || fpga_busy() ? STATUS_BUSY : 0);
         spin_unlock_irqrestore(&l.state_lock, flags);
         retval = put_user(value, (unsigned long __user *)arg);
         break;

      case CMD_RESET_ACC:
         spin_lock_irqsave(&l.state_lock, flags);
         if ((l.pending && !(l.status & STATUS_DONE)) ||
             fpga_busy()) {
            retval = -EBUSY;
         } else {
            fpga_ack_irq();
            l.status = 0;
            l.result = 0;
            l.pending = false;
         }
         spin_unlock_irqrestore(&l.state_lock, flags);
         break;

      default:
         retval = -EINVAL;
   }
   mutex_unlock(&l.data_mutex);
   return retval;
}

/* define which file operations are supported by the driver */
static const struct file_operations fpga_fops = {
   .owner          = THIS_MODULE,
   .llseek         = no_llseek,
   .read           = fpga_read1,
   .write          = fpga_write1,
   .unlocked_ioctl = fpga_ioctl1,
   .open           = fpga_open1,
   .release        = fpga_release1,
   .fasync         = fpga_fasync1,
};

/* =========================================================================
 * /proc entry
 */

/* Operations for /proc filesystem accesses */
static int proc_read_fpga_interrupt(struct seq_file *f, void *v)
{
  seq_printf(f, "Total number of interrupts %19i\n", READ_ONCE(interruptcount));

  return 0;
}

static int proc_open_fpga_interrupt(struct inode *inode, struct  file *file) {
  return single_open(file, proc_read_fpga_interrupt, NULL);
}

/* operations supported on the /proc entry */
static const struct proc_ops proc_ops = {
  .proc_open = proc_open_fpga_interrupt,
  .proc_read = seq_read,
  .proc_lseek = seq_lseek,
  .proc_release = single_release,
};


/* =========================================================================
 * Device handling
 */

/* /dev entry */
static struct miscdevice fpga_miscdev = {
        .minor =        MISC_DYNAMIC_MINOR,
        .name =         DRIVER_NAME,
        .fops =         &fpga_fops,
};


/* Probe and install the single supported device. */
static int fpga_drv_probe (struct platform_device *pdev) 
{
   void __iomem *base;
   struct device *dev = &pdev->dev;
   struct resource *r_mem; /* IO memory resources */

   int rv = -EBUSY;
   bool irq_installed = false;

   if (l.base)
      return -EBUSY;
   dev_info(dev, "FPGA Device Tree Probing\n");

   mutex_init(&l.data_mutex);
   spin_lock_init(&l.state_lock);

   // get, register and remap address region assigned to the device registers
   base = devm_platform_get_and_ioremap_resource(pdev, 0, &r_mem);
   if (IS_ERR(base)) {
     dev_err(dev, "fpga_drv: Unable to map FPGA registers.\n");
     rv = PTR_ERR(base);
     goto end;
   }

   if (resource_size(r_mem) < MAP_BYTES) {
      dev_err(dev, "Need a 256 KiB AXI segment and DT resource for operand windows\n");
      rv = -EINVAL;
      goto end;
   }
   l.base = base;
   l.base_start = r_mem->start;
   l.base_end = r_mem->end;


   dev_info(dev, "fpga_drv: 0x%08lx size 0x%08lx mapped to 0x%08lx\n", 
            l.base_start, l.base_end - l.base_start + 1, 
            (unsigned long)l.base);
   if (fpga_busy()) {
      rv = -EBUSY;
      goto end;
   }
   fpga_irq_disable(l.base);
   fpga_ack_irq();

   // get the interrupt assigned to the device
   l.irq = platform_get_irq(pdev, 0);
   if (l.irq < 0) {
     dev_info(dev, "fpga_drv: no IRQ found\n");
     rv = l.irq;
     goto end;
   } 

   // request interrupt from linux 
   rv = devm_request_irq(dev, l.irq, &fpga_int_handler, 0, DRIVER_NAME,  NULL);
   if (rv)
   {
      dev_err(dev, "fpga_drv: Can't get interrupt %d: %d\n", l.irq, rv);
      goto end;
   }

   irq_installed = true;
   rv = devm_add_action_or_reset(dev, fpga_irq_disable, l.base);
   if (rv)
      goto end;

   dev_info(dev, "fpga_drv: using interrupt %d\n", l.irq);

   // create /proc file system entry
   l.fpga_interrupt_file = proc_create(DRIVER_NAME, 0444, NULL, &proc_ops);
   if(l.fpga_interrupt_file == NULL)
   {
      dev_err(dev, "fpga_drv: create /proc entry returned NULL. ABORTING!\n");
      rv = -ENOMEM;
      goto end;
   }

   writel(IRQ_DONE, l.base + REG_IER_OFFSET);
   writel(1, l.base + REG_GIE_OFFSET);

   /* Publish only after mapping, locks and interrupt handling are ready. */
   rv = misc_register(&fpga_miscdev);
   if (rv) {
      remove_proc_entry(DRIVER_NAME, NULL);
      goto end;
   }

   // everything initialized
   dev_info(dev, "fpga_drv: %s %s Initialized\n", fpga_NAME, fpga_VERSION);
   return 0;

end:
   if (l.base) {
      fpga_irq_disable(l.base);
      if (irq_installed) {
         synchronize_irq(l.irq);
         devm_free_irq(dev, l.irq, NULL);
      }
      l.base = NULL;
   }
   return rv;  
}

/* remove driver from kernel */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
static void fpga_drv_remove(struct platform_device *pdev)
#else
static int fpga_drv_remove(struct platform_device *pdev)
#endif

{
   struct device *dev = &pdev->dev;

   fpga_irq_disable(l.base);
   synchronize_irq(l.irq);

   // de-register driver with kernel
   misc_deregister(&fpga_miscdev);

   // remove /proc entry
   remove_proc_entry(DRIVER_NAME, NULL);

   dev_info(dev, "fpga_drv: %s %s removed\n", fpga_NAME, fpga_VERSION);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
   return 0;
#endif
}

#ifdef CONFIG_OF
static struct of_device_id fpga_drv_of_match[] = {
        { .compatible = "ece382m,dotprod", },
        { /* end of list */ },
};
MODULE_DEVICE_TABLE(of, fpga_drv_of_match);
#else
# define fpga_drv_of_match NULL
#endif


static struct platform_device *fpga_dev = NULL;

/* Kernel driver data structure */
static struct platform_driver fpga_driver = {
  .driver = {
    .name = DRIVER_NAME,
    .owner = THIS_MODULE,
    .of_match_table = fpga_drv_of_match,
    .suppress_bind_attrs = true, /* Prevent manual unbind with open files. */
  },
  .probe          = fpga_drv_probe,
  .remove         = fpga_drv_remove,
};

#ifdef NO_DTS
/* Resources assigned to device */ 
static struct resource fpga_resources[] = {
  {
    .start= FPGA_BASE,
    .end=   FPGA_BASE+MAP_BYTES-1,
    .flags= IORESOURCE_MEM,
    .name= "io-memory"
    },
  {
    .start= 0,
    .end= 0,
    .flags= IORESOURCE_IRQ,
    .name= "irq",
    }
};
#endif

/* =========================================================================
 * Module handling
 */

/* Load and initialize module */
static int __init fpga_init_module(void)
{
   int rv = 0;

#ifdef DEBUG
   printk("FPGA Interface Module\n");
   printk(KERN_INFO "\nfpga_drv: FPGA Driver Loading.\n");
#endif

   // register driver with kernel
   rv = platform_driver_register(&fpga_driver);
   if (rv) return rv;

#ifdef NO_DTS
   // if we are asked to install the device, register (and hence probe) it
   if(install) {
     if (irq_number < 0) {
       platform_driver_unregister(&fpga_driver);
       return -EINVAL;
     }
     fpga_resources[1].start = irq_number;
     fpga_resources[1].end = irq_number;
     fpga_dev = platform_device_register_simple(DRIVER_NAME, -1, 
                                                &(fpga_resources[0]), 2);
     if (IS_ERR(fpga_dev)) {
       rv = PTR_ERR(fpga_dev);
       platform_driver_unregister(&fpga_driver);
       return rv;
     }
   }
#endif   

   return 0;
}

/* Unload module */
static void __exit fpga_cleanup_module(void)
{
  if(fpga_dev) platform_device_unregister(fpga_dev);
  platform_driver_unregister(&fpga_driver);
#ifdef DEBUG
  printk(KERN_ALERT "fpga_drv: Unloading.\n");
#endif
}

module_init(fpga_init_module);
module_exit(fpga_cleanup_module);

