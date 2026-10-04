/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include "aesd_ioctl.h"
#include "aesd-circular-buffer.h"
#include "aesdchar.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Your Name Here"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev;
    PDEBUG("open");
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;

    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    struct aesd_buffer_entry * entry;
    size_t entry_offset = 0;
    unsigned long bytes_to_copy = 0;
    unsigned long uncopied_bytes;

    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);
    if (mutex_lock_interruptible(&aesd_device.lock))
        return -ERESTARTSYS;

    entry = aesd_circular_buffer_find_entry_offset_for_fpos(
                &aesd_device.circular_buffer,
                *f_pos,
                &entry_offset);

    if (!entry) {
        /* EOF reached or position out of bounds */
        mutex_unlock(&aesd_device.lock);
        return 0;
    }

    /* Calculate maximum available bytes in the found entry starting at entry_offset */
    bytes_to_copy = entry->size - entry_offset;
    if (bytes_to_copy > count) {
        bytes_to_copy = count;
    }

    uncopied_bytes = copy_to_user(buf, entry->buffptr + entry_offset, bytes_to_copy);
    retval = (ssize_t)(bytes_to_copy - uncopied_bytes);

    *f_pos += retval;
    mutex_unlock(&aesd_device.lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    struct aesd_dev *dev = filp->private_data;
    char *kbuf = NULL;
    char *newline_ptr = NULL;
    const char *evicted_ptr = NULL;
    unsigned long uncopied_bytes = 0;
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);
    if (count == 0)
        return 0;

    kbuf = kmalloc(count, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    uncopied_bytes = copy_from_user(kbuf, buf, count);
    if (uncopied_bytes) {
        kfree(kbuf);
        return -EFAULT;
    }

    if (mutex_lock_interruptible(&dev->lock)) {
        kfree(kbuf);
        return -ERESTARTSYS;
    }

    /* Reallocate partial buffer to accumulate incoming write data */
    dev->partial_entry.buffptr = krealloc(
        (void *)dev->partial_entry.buffptr,
        dev->partial_entry.size + count,
        GFP_KERNEL
    );

    if (!dev->partial_entry.buffptr) {
        mutex_unlock(&dev->lock);
        kfree(kbuf);
        return -ENOMEM;
    }

    /* Append newly received chunk to the partial write entry */
    memcpy((char *)dev->partial_entry.buffptr + dev->partial_entry.size, kbuf, count);
    dev->partial_entry.size += count;
    kfree(kbuf);
    retval = count;

    /* Check if a newline character terminates the command string */
    newline_ptr = memchr(dev->partial_entry.buffptr, '\n', dev->partial_entry.size);

    if (newline_ptr) {
        /* If the circular buffer is full, the entry at in_offs will be overwritten */
        if (dev->circular_buffer.full) {
            evicted_ptr = dev->circular_buffer.entry[dev->circular_buffer.in_offs].buffptr;
        }

        aesd_circular_buffer_add_entry(&dev->circular_buffer, &dev->partial_entry);

        /* Free the evicted entry's buffer if we overwrote an old entry */
        if (evicted_ptr) {
            kfree(evicted_ptr);
            evicted_ptr = NULL;
        }

        /* Reset partial entry tracker for next write */
        dev->partial_entry.buffptr = NULL;
        dev->partial_entry.size = 0;
    }

    mutex_unlock(&dev->lock);
    return retval;
}

loff_t aesd_llseek(struct file *filp, loff_t offset, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    loff_t total_size = 0;
    uint8_t index;
    struct aesd_buffer_entry *entry;
    loff_t retval;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    /* Calculate total byte size of all valid entries in the circular buffer */
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->circular_buffer, index) {
        if (entry->buffptr != NULL) {
            total_size += entry->size;
        }
    }

    /* Use kernel helper to calculate and validate the new offset */
    retval = fixed_size_llseek(filp, offset, whence, total_size);

    mutex_unlock(&dev->lock);
    return retval;
}

long aesd_adjust_file_offset(struct file *filp, uint32_t write_cmd, uint32_t write_cmd_offset)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    uint8_t index;
    loff_t new_fpos = 0;
    uint8_t valid_entries = 0;
    int i;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    /* 1. Count valid entries in circular buffer */
    if (dev->circular_buffer.full) {
        valid_entries = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    } else if (dev->circular_buffer.in_offs >= dev->circular_buffer.out_offs) {
        valid_entries = dev->circular_buffer.in_offs - dev->circular_buffer.out_offs;
    } else {
        valid_entries = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED - (dev->circular_buffer.out_offs - dev->circular_buffer.in_offs);
    }

    if (write_cmd >= valid_entries) {
        mutex_unlock(&dev->lock);
        PDEBUG("Invalid write_cmd %u, valid entries %u", write_cmd, valid_entries);
        return -EINVAL;
    }

    /* 2. Validate write_cmd index */
    if (write_cmd >= AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED) {
        mutex_unlock(&dev->lock);
        PDEBUG("Invalid write_cmd %u, valid entries %u", write_cmd, valid_entries);
        return -EINVAL;
    }

    /* 3. Validate write_cmd_offset within the targeted entry */
    index = (dev->circular_buffer.out_offs + write_cmd) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    if (write_cmd_offset >= dev->circular_buffer.entry[index].size) {
        mutex_unlock(&dev->lock);
        PDEBUG("Invalid write_cmd_offset %u for entry size %zu", write_cmd_offset, dev->circular_buffer.entry[index].size);
        return -EINVAL;
    }

    /* 4. Calculate absolute byte position (sum lengths of preceding entries) */
    for (i = 0; i < write_cmd; i++) {
        uint8_t pos = (dev->circular_buffer.out_offs + i) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
        new_fpos += dev->circular_buffer.entry[pos].size;
    }
    new_fpos += write_cmd_offset;

    /* 5. Update file position */
    filp->f_pos = new_fpos;
    PDEBUG("ioctl updated filp->f_pos to %lld", filp->f_pos);

    mutex_unlock(&dev->lock);
    return 0;
}

long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_seekto seekto;

    if (cmd != AESDCHAR_IOCSEEKTO)
        return -ENOTTY;

    if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto)))
        return -EFAULT;

    PDEBUG("ioctl received with offset %u", seekto.write_cmd_offset);

    return aesd_adjust_file_offset(filp, seekto.write_cmd, seekto.write_cmd_offset);
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
    .llseek =   aesd_llseek,
    .unlocked_ioctl = aesd_ioctl,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /* Initialize synchronization primitives and buffer data structures */
    mutex_init(&aesd_device.lock);
    aesd_circular_buffer_init(&aesd_device.circular_buffer);
    aesd_device.partial_entry.buffptr = NULL;
    aesd_device.partial_entry.size = 0;

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    uint8_t index;
    struct aesd_buffer_entry *entry;
    cdev_del(&aesd_device.cdev);

    /* Free all dynamically allocated memory remaining in the circular buffer */
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.circular_buffer, index) {
        if (entry->buffptr) {
            kfree(entry->buffptr);
            entry->buffptr = NULL;
        }
    }

    /* Free any partial un-terminated command buffer if present */
    if (aesd_device.partial_entry.buffptr) {
        kfree(aesd_device.partial_entry.buffptr);
        aesd_device.partial_entry.buffptr = NULL;
    }

    mutex_destroy(&aesd_device.lock);

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
