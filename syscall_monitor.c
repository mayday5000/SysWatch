// SPDX-License-Identifier: GPL-2.0 OR MIT
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/tracepoint.h>
#include <linux/version.h>
#include <linux/poll.h>
#include <linux/ktime.h>
#include <linux/string.h>
#include <linux/capability.h>
#include <linux/mutex.h>
#include <asm/syscall.h>

#include "syscall_monitor_shared.h"

/*
 * syscall_monitor.c
 * Purpose: the kernel module. Registers the sys_enter tracepoint, writes
 * one packed record per syscall into a broadcast ring, and serves that
 * ring to every open of /dev/syscall_monitor. Each open has its own
 * cursor and its own ioctl filters. The probe does not sleep.
 */

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("Kernel Architecture Team");
MODULE_DESCRIPTION("Multi-Reader Parameter-Hooking Tracepoint Monitor");
MODULE_VERSION("1.1");

#define DEVICE_NAME      "syscall_monitor"
#define CLASS_NAME       "syscall_mon"

/*
 * Per-open state. lock serialises ioctl() and read() on the same file
 * (threads or a dup()ed/inherited fd): it guards the filters, the read
 * cursor and snap, so n_sys can never exceed SM_MAX_NR_FILTERS.
 */
struct sm_priv {
    struct mutex lock;
    unsigned int read_index;
    unsigned long last_seq;
    struct sm_event *snap;
    pid_t filter_pid;
    pid_t exclude_pid;
    int n_sys;
    sm_s32 sys_ids[SM_MAX_NR_FILTERS];
};

static struct {
    struct sm_event ring[SM_EVENT_RING_SIZE];
    unsigned int head;
    spinlock_t lock;
    wait_queue_head_t waitq;
    unsigned long seq;
    dev_t dev_num;
    struct cdev cdev;
    struct class *cls;
    struct device *dev;
    struct tracepoint *tp_sys_enter;
} mon_ctx;

static void find_sys_enter_tp(struct tracepoint *tp, void *priv)
{
    if (tp && tp->name && strcmp(tp->name, "sys_enter") == 0)
        *(struct tracepoint **)priv = tp;
}

static void probe_sys_enter(void *data, struct pt_regs *regs, long id)
{
    unsigned long flags;
    struct sm_event *ev;
    unsigned long args[6] = { 0 };
    char path[SM_ARG_STR_LEN];
    sm_u64 arg_a = 0, arg_b = 0;
    pid_t current_pid = current->pid;

    (void)data;
    if (!regs)
        return;

    memset(path, 0, sizeof(path));

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)
    syscall_get_arguments(current, regs, args);
#else
    syscall_get_arguments(current, regs, 0, 6, args);
#endif

    if (id == SM_NR_OPENAT) {
        arg_a = (sm_u64)args[0];
        arg_b = (sm_u64)args[2];
        if (args[1]) {
            long res;

            pagefault_disable();
            res = strncpy_from_user(path, (const char __user *)args[1],
                                    SM_ARG_STR_LEN - 1);
            pagefault_enable();
            if (res < 0)
                strscpy(path, "[EFAULT]", sizeof(path));
        }
    } else if (id == SM_NR_MMAP) {
        arg_a = (sm_u64)args[0];
        arg_b = (sm_u64)args[1];
        strscpy(path, "mmap", sizeof(path));
    } else if (id == SM_NR_EXECVE) {
        arg_a = (sm_u64)args[1];
        arg_b = (sm_u64)args[2];
        if (args[0]) {
            long res;

            pagefault_disable();
            res = strncpy_from_user(path, (const char __user *)args[0],
                                    SM_ARG_STR_LEN - 1);
            pagefault_enable();
            if (res < 0)
                strscpy(path, "[EFAULT]", sizeof(path));
        }
    } else {
        arg_a = (sm_u64)args[0];
        arg_b = (sm_u64)args[1];
    }

    spin_lock_irqsave(&mon_ctx.lock, flags);
    ev = &mon_ctx.ring[mon_ctx.head];
    ev->pid = (sm_s32)current_pid;
    ev->nr = (sm_s32)id;
    ev->ts_ns = (sm_u64)ktime_get_real_ns();
    ev->arg_a = arg_a;
    ev->arg_b = arg_b;
    strscpy(ev->comm, current->comm, sizeof(ev->comm));
    memcpy(ev->arg_str, path, SM_ARG_STR_LEN);
    mon_ctx.head = (mon_ctx.head + 1) & SM_EVENT_RING_MASK;
    mon_ctx.seq++;
    spin_unlock_irqrestore(&mon_ctx.lock, flags);
    wake_up_interruptible(&mon_ctx.waitq);
}

static long sm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct sm_priv *priv = filp->private_data;
    sm_s32 val32 = 0;

    if (!priv)
        return -EFAULT;

    switch (cmd) {
    case SET_FILTER_PID:
    case SET_EXCLUDE_PID:
    case SET_FILTER_SYSCALL:
    case ADD_FILTER_SYSCALL:
    case DEL_FILTER_SYSCALL:
        if (copy_from_user(&val32, (sm_s32 __user *)arg, sizeof(val32)))
            return -EFAULT;
        break;
    case CLEAR_FILTERS:
        break;
    default:
        return -EINVAL;
    }

    if (mutex_lock_interruptible(&priv->lock))
        return -ERESTARTSYS;

    switch (cmd) {
    case SET_FILTER_PID:
        WRITE_ONCE(priv->filter_pid, (pid_t)val32);
        break;
    case SET_EXCLUDE_PID:
        WRITE_ONCE(priv->exclude_pid, (pid_t)val32);
        break;
    case SET_FILTER_SYSCALL:
        priv->sys_ids[0] = val32;
        priv->n_sys = 1;
        break;
    case ADD_FILTER_SYSCALL:
        {
            int i, found = 0;
            for (i = 0; i < priv->n_sys; i++) {
                if (priv->sys_ids[i] == val32)
                    found = 1;
            }
            if (!found && priv->n_sys < SM_MAX_NR_FILTERS)
                priv->sys_ids[priv->n_sys++] = val32;
        }
        break;
    case DEL_FILTER_SYSCALL:
        {
            int i, j = 0;
            for (i = 0; i < priv->n_sys; i++) {
                if (priv->sys_ids[i] != val32)
                    priv->sys_ids[j++] = priv->sys_ids[i];
            }
            priv->n_sys = j;
        }
        break;
    case CLEAR_FILTERS:
        WRITE_ONCE(priv->filter_pid, 0);
        WRITE_ONCE(priv->exclude_pid, 0);
        priv->n_sys = 0;
        break;
    }
    mutex_unlock(&priv->lock);
    return 0;
}

/*
 * The stream shows every process's pid, comm, opened paths and exec'd
 * filenames, so opening it needs the same privilege as perf tracing:
 * CAP_PERFMON (5.8+) or CAP_SYS_ADMIN. The node itself is root-only 0600.
 */
static bool sm_capable(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
    return perfmon_capable();
#else
    return capable(CAP_SYS_ADMIN);
#endif
}

static int sm_open(struct inode *inode, struct file *filp)
{
    struct sm_priv *priv;
    unsigned long flags;

    if (!sm_capable())
        return -EPERM;
    priv = kzalloc(sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    priv->snap = kmalloc_array(SM_EVENT_RING_SIZE, SM_EVENT_SIZE, GFP_KERNEL);
    if (!priv->snap) {
        kfree(priv);
        return -ENOMEM;
    }

    mutex_init(&priv->lock);
    priv->filter_pid = 0;
    priv->exclude_pid = 0;
    priv->n_sys = 0;

    spin_lock_irqsave(&mon_ctx.lock, flags);
    priv->read_index = mon_ctx.head;
    priv->last_seq = mon_ctx.seq;
    spin_unlock_irqrestore(&mon_ctx.lock, flags);

    filp->private_data = priv;
    /* No seeking. Replaces .llseek = no_llseek, which 6.12 removed. */
    return nonseekable_open(inode, filp);
}

static int sm_release(struct inode *inode, struct file *filp)
{
    struct sm_priv *priv = filp->private_data;

    (void)inode;
    if (priv) {
        mutex_destroy(&priv->lock);
        kfree(priv->snap);
        kfree(priv);
    }
    return 0;
}

static bool sm_event_wanted(const struct sm_priv *priv, const struct sm_event *ev)
{
    pid_t f_pid = READ_ONCE(priv->filter_pid);
    pid_t ex_pid = READ_ONCE(priv->exclude_pid);
    int i, n_sys = priv->n_sys;

    if (ex_pid != 0 && ev->pid == (sm_s32)ex_pid)
        return false;
    if (f_pid != 0 && ev->pid != (sm_s32)f_pid)
        return false;
    if (n_sys <= 0)
        return true;
    for (i = 0; i < n_sys; i++) {
        if (ev->nr == priv->sys_ids[i])
            return true;
    }
    return false;
}

static ssize_t sm_read(struct file *filp, char __user *ubuf, size_t count, loff_t *ppos)
{
    struct sm_priv *priv = filp->private_data;
    unsigned long flags;
    unsigned int local_head, n;
    size_t n_out, nbytes;
    int ret = 0;

    (void)ppos;
    if (!priv || !priv->snap)
        return -EFAULT;

    if (!(filp->f_flags & O_NONBLOCK)) {
        ret = wait_event_interruptible(mon_ctx.waitq,
                                       READ_ONCE(mon_ctx.seq) != priv->last_seq);
        if (ret)
            return ret;
    } else if (READ_ONCE(mon_ctx.seq) == priv->last_seq) {
        return -EAGAIN;
    }

    if (mutex_lock_interruptible(&priv->lock))
        return -ERESTARTSYS;

    spin_lock_irqsave(&mon_ctx.lock, flags);
    local_head = mon_ctx.head;

    /* Slow reader lapped by a full ring: drop history, keep last N-1. */
    if (mon_ctx.seq - priv->last_seq >= SM_EVENT_RING_SIZE)
        priv->read_index = (local_head - (SM_EVENT_RING_SIZE - 1)) & SM_EVENT_RING_MASK;

    priv->last_seq = mon_ctx.seq;
    n = 0;
    while (priv->read_index != local_head && n < SM_EVENT_RING_SIZE) {
        const struct sm_event *src = &mon_ctx.ring[priv->read_index];

        priv->read_index = (priv->read_index + 1) & SM_EVENT_RING_MASK;
        if (!sm_event_wanted(priv, src))
            continue;
        priv->snap[n++] = *src;
    }
    spin_unlock_irqrestore(&mon_ctx.lock, flags);

    n_out = count / SM_EVENT_SIZE;
    if (n_out > n)
        n_out = n;
    nbytes = n_out * SM_EVENT_SIZE;

    if (nbytes == 0 && count > 0 && n > 0)
        ret = -ENOBUFS;
    else if (nbytes > 0 && copy_to_user(ubuf, priv->snap, nbytes))
        ret = -EFAULT;
    mutex_unlock(&priv->lock);

    return ret ? ret : (ssize_t)nbytes;
}

static __poll_t sm_poll(struct file *filp, poll_table *wait)
{
    struct sm_priv *priv = filp->private_data;
    __poll_t mask = 0;

    poll_wait(filp, &mon_ctx.waitq, wait);
    if (priv && READ_ONCE(mon_ctx.seq) != priv->last_seq)
        mask |= EPOLLIN | EPOLLRDNORM;
    return mask;
}

static const struct file_operations sm_fops = {
    .owner          = THIS_MODULE,
    .open           = sm_open,
    .release        = sm_release,
    .read           = sm_read,
    .poll           = sm_poll,
    .unlocked_ioctl = sm_ioctl,
};

static int __init syscall_monitor_init(void)
{
    int ret;

    spin_lock_init(&mon_ctx.lock);
    init_waitqueue_head(&mon_ctx.waitq);
    mon_ctx.head = 0;
    mon_ctx.seq = 0;

    ret = alloc_chrdev_region(&mon_ctx.dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0)
        return ret;

    cdev_init(&mon_ctx.cdev, &sm_fops);
    mon_ctx.cdev.owner = THIS_MODULE;
    if (cdev_add(&mon_ctx.cdev, mon_ctx.dev_num, 1) < 0)
        goto err_region;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    mon_ctx.cls = class_create(CLASS_NAME);
#else
    mon_ctx.cls = class_create(THIS_MODULE, CLASS_NAME);
#endif
    if (IS_ERR(mon_ctx.cls))
        goto err_cdev;

    mon_ctx.dev = device_create(mon_ctx.cls, NULL, mon_ctx.dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(mon_ctx.dev))
        goto err_class;

    for_each_kernel_tracepoint(find_sys_enter_tp, &mon_ctx.tp_sys_enter);
    if (!mon_ctx.tp_sys_enter)
        goto err_device;

    ret = tracepoint_probe_register(mon_ctx.tp_sys_enter, (void *)probe_sys_enter, NULL);
    if (ret)
        goto err_device;

    pr_info("syscall_monitor: loaded (broadcast ring %u, rec %u)\n",
            SM_EVENT_RING_SIZE, (unsigned int)SM_EVENT_SIZE);
    return 0;

err_device:
    device_destroy(mon_ctx.cls, mon_ctx.dev_num);
err_class:
    class_destroy(mon_ctx.cls);
err_cdev:
    cdev_del(&mon_ctx.cdev);
err_region:
    unregister_chrdev_region(mon_ctx.dev_num, 1);
    return -ENODEV;
}

static void __exit syscall_monitor_exit(void)
{
    if (mon_ctx.tp_sys_enter) {
        tracepoint_probe_unregister(mon_ctx.tp_sys_enter, (void *)probe_sys_enter, NULL);
        tracepoint_synchronize_unregister();
    }
    device_destroy(mon_ctx.cls, mon_ctx.dev_num);
    class_destroy(mon_ctx.cls);
    cdev_del(&mon_ctx.cdev);
    unregister_chrdev_region(mon_ctx.dev_num, 1);
    pr_info("syscall_monitor: unloaded\n");
}

module_init(syscall_monitor_init);
module_exit(syscall_monitor_exit);
