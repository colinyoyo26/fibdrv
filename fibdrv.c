#include <linux/cdev.h>
#include <linux/compiler.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>


#include "bignum.h"

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("Fibonacci engine driver");
MODULE_VERSION("0.1");

#define DEV_FIBONACCI_NAME "fibonacci"

/* MAX_LENGTH is set to 92 because
 * ssize_t can't fit the number > 92
 */
#define MAX_LENGTH 10000

static dev_t fib_dev = 0;
static struct cdev *fib_cdev;
static struct class *fib_class;
static DEFINE_MUTEX(fib_mutex);
struct kobject *kobj_ref;

#ifndef BN
static long long fib_sequence(long long k)
{
    if (unlikely(!k))
        return 0;

    long long fcur = 1, fnext = 1;

    /* off is offset of checking bit from MSB */
    for (int off = __builtin_clzll(k) + 1; likely(off < BITS); off++) {
        long long t1 = fcur * ((fnext << 1) - fcur);
        long long t2 = fcur * fcur + fnext * fnext;
        long long mask = ((k >> (BITS - 1 - off)) & 1) - 1;
        fcur = (t1 & mask) + (t2 & ~mask);
        fnext = t2 + (t1 & ~mask);
    }

    return fcur;
}
#else

#ifndef FAST
static char *fib_sequence(long long k)
{
    bn fcur, fnext, r;
    bn_init(&fcur);
    bn_init(&fnext);
    bn_init(&r);
    bn_assign(&fcur, 0);
    bn_assign(&fnext, 1);

    for (int i = 0; i < k; i++) {
        bn_add(&r, &fnext, &fcur);
        bn_swap(&fcur, &fnext);
        bn_swap(&fnext, &r);
    }

    return bn_hex(&fcur);
}
#else
static char *fib_sequence(long long k)
{
    unsigned long long mask = 1ull << (BITS - 1);
    unsigned int off = __builtin_clzll(k) + 1;
    mask >>= off;

    bn fcur, fnext, t1, t2;
    bn fnext2, fcur_sqrt, fnext_sqrt, tem;
    bn_init(&fcur);
    bn_init(&fnext);
    bn_init(&fnext2);
    bn_init(&t1);
    bn_init(&t2);
    bn_init(&tem);
    bn_init(&fcur_sqrt);
    bn_init(&fnext_sqrt);

    bn_assign(&fcur, 1);
    bn_assign(&fnext, 1);

    if (unlikely(k <= 1)) {
        bn_assign(&fcur, k);
        return bn_hex(&fcur);
    }

    for (; mask; mask >>= 1) {
        bn_sll(&fnext2, &fnext, 1);
        bn_sub(&tem, &fnext2, &fcur);
        bn_mul(&t1, &tem, &fcur);
        bn_mul(&fcur_sqrt, &fcur, &fcur);
        bn_mul(&fnext_sqrt, &fnext, &fnext);
        bn_add(&t2, &fcur_sqrt, &fnext_sqrt);
        bn_swap(&fcur, &t1);
        bn_swap(&fnext, &t2);
        if (k & mask) {
            bn_add(&t1, &fcur, &fnext);
            bn_swap(&fcur, &fnext);
            bn_swap(&fnext, &t1);
        }
    }

    return bn_hex(&fcur);
}
#endif
#endif

static ktime_t kt;
static long long kt_ns;

static ssize_t show(struct kobject *kobj,
                    struct kobj_attribute *attr,
                    char *buf)
{
    kt_ns = ktime_to_ns(kt);
    return snprintf(buf, 16, "%lld\n", kt_ns);
}

static ssize_t store(struct kobject *kobj,
                     struct kobj_attribute *attr,
                     const char *buf,
                     size_t count)
{
    int ret, n_th;
    ret = kstrtoint(buf, 10, &n_th);
    kt = ktime_get();
    fib_sequence(n_th);
    kt = ktime_sub(ktime_get(), kt);
    if (ret < 0)
        return ret;
    return count;
}

static struct kobj_attribute ktime_attr = __ATTR(kt_ns, 0664, show, store);

static struct attribute *attrs[] = {
    &ktime_attr.attr,
    NULL,
};

static struct attribute_group attr_group = {
    .attrs = attrs,
};

static int fib_open(struct inode *inode, struct file *file)
{
    if (!mutex_trylock(&fib_mutex)) {
        printk(KERN_ALERT "fibdrv is in use");
        return -EBUSY;
    }
    return 0;
}

static int fib_release(struct inode *inode, struct file *file)
{
    mutex_unlock(&fib_mutex);
    return 0;
}

/* calculate the fibonacci number at given offset */
static ssize_t fib_read(struct file *file,
                        char *buf,
                        size_t size,
                        loff_t *offset)
{
    kt = ktime_get();
    char *fibnum = fib_sequence(*offset);
    kt = ktime_sub(ktime_get(), kt);
    int len = strlen(fibnum);
    copy_to_user(buf, fibnum, (len + 1) * sizeof(char));
    kfree(fibnum);
    return len;
}

/* write operation is skipped */
static ssize_t fib_write(struct file *file,
                         const char *buf,
                         size_t size,
                         loff_t *offset)
{
    return 1;
}

static loff_t fib_device_lseek(struct file *file, loff_t offset, int orig)
{
    loff_t new_pos = 0;
    switch (orig) {
    case 0: /* SEEK_SET: */
        new_pos = offset;
        break;
    case 1: /* SEEK_CUR: */
        new_pos = file->f_pos + offset;
        break;
    case 2: /* SEEK_END: */
        new_pos = MAX_LENGTH - offset;
        break;
    }

    if (new_pos > MAX_LENGTH)
        new_pos = MAX_LENGTH;  // max case
    if (new_pos < 0)
        new_pos = 0;        // min case
    file->f_pos = new_pos;  // This is what we'll use now
    return new_pos;
}

const struct file_operations fib_fops = {
    .owner = THIS_MODULE,
    .read = fib_read,
    .write = fib_write,
    .open = fib_open,
    .release = fib_release,
    .llseek = fib_device_lseek,
};

static int __init init_fib_dev(void)
{
    int rc = 0;

    mutex_init(&fib_mutex);

    // Let's register the device
    // This will dynamically allocate the major number
    rc = alloc_chrdev_region(&fib_dev, 0, 1, DEV_FIBONACCI_NAME);

    if (rc < 0) {
        printk(KERN_ALERT
               "Failed to register the fibonacci char device. rc = %i",
               rc);
        return rc;
    }

    fib_cdev = cdev_alloc();
    if (fib_cdev == NULL) {
        printk(KERN_ALERT "Failed to alloc cdev");
        rc = -1;
        goto failed_cdev;
    }
    cdev_init(fib_cdev, &fib_fops);
    rc = cdev_add(fib_cdev, fib_dev, 1);

    if (rc < 0) {
        printk(KERN_ALERT "Failed to add cdev");
        rc = -2;
        goto failed_cdev;
    }

    kobj_ref = kobject_create_and_add("kobj_ref", kernel_kobj);
    if (!kobj_ref)
        return -ENOMEM;

    if (sysfs_create_group(kobj_ref, &attr_group))
        kobject_put(kobj_ref);

    fib_class = class_create(THIS_MODULE, DEV_FIBONACCI_NAME);

    if (!fib_class) {
        printk(KERN_ALERT "Failed to create device class");
        rc = -3;
        goto failed_class_create;
    }

    if (!device_create(fib_class, NULL, fib_dev, NULL, DEV_FIBONACCI_NAME)) {
        printk(KERN_ALERT "Failed to create device");
        rc = -4;
        goto failed_device_create;
    }
    return rc;
failed_device_create:
    class_destroy(fib_class);
failed_class_create:
    cdev_del(fib_cdev);
failed_cdev:
    unregister_chrdev_region(fib_dev, 1);
    return rc;
}

static void __exit exit_fib_dev(void)
{
    mutex_destroy(&fib_mutex);
    device_destroy(fib_class, fib_dev);
    class_destroy(fib_class);
    cdev_del(fib_cdev);
    unregister_chrdev_region(fib_dev, 1);
    kobject_put(kobj_ref);
}

module_init(init_fib_dev);
module_exit(exit_fib_dev);
