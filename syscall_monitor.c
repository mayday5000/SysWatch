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
    unsigned long next_seq;     /* seq of the next record to examine */
    struct sm_event *snap;
    pid_t filter_pid;
    pid_t exclude_pid;
    int n_sys;
    sm_s32 sys_ids[SM_MAX_NR_FILTERS];
};

/*
 * Broadcast ring. seq counts records ever written; record k lives in
 * ring[k & SM_EVENT_RING_MASK]. lock is a raw spinlock because the probe
 * runs with preemption disabled (a spinlock_t sleeps on PREEMPT_RT), and
 * it is only ever held for one 176-byte copy on either side.
 */
static struct {
    struct sm_event ring[SM_EVENT_RING_SIZE];
    raw_spinlock_t lock;
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

/*
 * Copy a user string without faulting. The probe runs in atomic context,
 * so a page that is not resident yields "[EFAULT]" instead of a sleep.
 */
static void sm_copy_user_str(char *dst, unsigned long uptr)
{
    long res;

    if (!uptr)
        return;
    pagefault_disable();
    res = strncpy_from_user(dst, (const char __user *)uptr, SM_ARG_STR_LEN - 1);
    pagefault_enable();
    if (res < 0) {
        memset(dst, 0, SM_ARG_STR_LEN);
        strscpy(dst, "[EFAULT]", SM_ARG_STR_LEN);
    }
}

static void probe_sys_enter(void *data, struct pt_regs *regs, long id)
{
    unsigned long flags;
    struct sm_event ev;
    unsigned long args[6] = { 0 };

    (void)data;
    if (!regs)
        return;

    /* Build the record on the stack, outside the lock. Zeroing it first
     * means no stale bytes from an older record ever reach userspace. */
    memset(&ev, 0, sizeof(ev));

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)
    syscall_get_arguments(current, regs, args);
#else
    syscall_get_arguments(current, regs, 0, 6, args);
#endif

    ev.pid = (sm_s32)current->pid;
    ev.nr = (sm_s32)id;
    ev.ts_ns = (sm_u64)ktime_get_real_ns();
    strscpy(ev.comm, current->comm, sizeof(ev.comm));

    if (id == SM_NR_OPENAT) {
        ev.arg_a = (sm_u64)args[0];
        ev.arg_b = (sm_u64)args[2];
        sm_copy_user_str(ev.arg_str, args[1]);
    } else if (id == SM_NR_MMAP) {
        ev.arg_a = (sm_u64)args[0];
        ev.arg_b = (sm_u64)args[1];
        strscpy(ev.arg_str, "mmap", sizeof(ev.arg_str));
    } else if (id == SM_NR_EXECVE) {
        ev.arg_a = (sm_u64)args[1];
        ev.arg_b = (sm_u64)args[2];
        sm_copy_user_str(ev.arg_str, args[0]);
    } else {
        ev.arg_a = (sm_u64)args[0];
        ev.arg_b = (sm_u64)args[1];
    }

    raw_spin_lock_irqsave(&mon_ctx.lock, flags);
    mon_ctx.ring[mon_ctx.seq & SM_EVENT_RING_MASK] = ev;
    WRITE_ONCE(mon_ctx.seq, mon_ctx.seq + 1);
    raw_spin_unlock_irqrestore(&mon_ctx.lock, flags);

    /* Only touch the wait queue lock when a reader is actually waiting. */
    if (wq_has_sleeper(&mon_ctx.waitq))
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

    priv->next_seq = READ_ONCE(mon_ctx.seq);

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

/*
 * Advance the cursor of a reader lapped by the producer: everything older
 * than the last SM_EVENT_RING_SIZE records has been overwritten.
 * Called with mon_ctx.lock held.
 */
static void sm_catch_up(struct sm_priv *priv)
{
    if (mon_ctx.seq - priv->next_seq > SM_EVENT_RING_SIZE)
        priv->next_seq = mon_ctx.seq - SM_EVENT_RING_SIZE;
}

/*
 * Copy up to max_out wanted records into priv->snap and return how many.
 * Unwanted records are consumed. Only records that existed on entry are
 * examined, so the loop is bounded even under a syscall storm, and
 * mon_ctx.lock is taken once per record so IRQs are never off for long.
 * Called with priv->lock held.
 */
static unsigned int sm_collect(struct sm_priv *priv, unsigned int max_out)
{
    unsigned long flags, end = READ_ONCE(mon_ctx.seq);
    unsigned int n = 0;

    while (n < max_out) {
        struct sm_event *dst = &priv->snap[n];

        raw_spin_lock_irqsave(&mon_ctx.lock, flags);
        sm_catch_up(priv);
        if ((long)(end - priv->next_seq) <= 0) {
            raw_spin_unlock_irqrestore(&mon_ctx.lock, flags);
            break;
        }
        *dst = mon_ctx.ring[priv->next_seq & SM_EVENT_RING_MASK];
        priv->next_seq++;
        raw_spin_unlock_irqrestore(&mon_ctx.lock, flags);

        if (sm_event_wanted(priv, dst))
            n++;
    }
    return n;
}

/*
 * True when a record this reader wants is waiting. Unwanted records in
 * front of it (for example the reader's own poll()/read() syscalls, which
 * SET_EXCLUDE_PID hides) are skipped, so they no longer make the device
 * look readable and the reader no longer wakes itself in a loop.
 * Called with priv->lock held.
 */
static bool sm_pending(struct sm_priv *priv)
{
    unsigned long flags, end = READ_ONCE(mon_ctx.seq);
    struct sm_event ev;

    for (;;) {
        raw_spin_lock_irqsave(&mon_ctx.lock, flags);
        sm_catch_up(priv);
        if ((long)(end - priv->next_seq) <= 0) {
            raw_spin_unlock_irqrestore(&mon_ctx.lock, flags);
            return false;
        }
        ev = mon_ctx.ring[priv->next_seq & SM_EVENT_RING_MASK];
        raw_spin_unlock_irqrestore(&mon_ctx.lock, flags);

        if (sm_event_wanted(priv, &ev))
            return true;
        priv->next_seq++;
    }
}

static ssize_t sm_read(struct file *filp, char __user *ubuf, size_t count, loff_t *ppos)
{
    struct sm_priv *priv = filp->private_data;
    unsigned int max_out, n;
    unsigned long seen;
    ssize_t ret;

    (void)ppos;
    if (!priv || !priv->snap)
        return -EFAULT;
    if (count == 0)
        return 0;
    /* Checked before anything is consumed, so no record is lost. */
    if (count < SM_EVENT_SIZE)
        return -ENOBUFS;

    max_out = (unsigned int)min_t(size_t, count / SM_EVENT_SIZE, SM_EVENT_RING_SIZE);

    for (;;) {
        if (mutex_lock_interruptible(&priv->lock))
            return -ERESTARTSYS;
        /* Never takes more records than fit in the caller's buffer. */
        n = sm_collect(priv, max_out);
        if (n) {
            size_t nbytes = (size_t)n * SM_EVENT_SIZE;

            ret = copy_to_user(ubuf, priv->snap, nbytes) ? -EFAULT : (ssize_t)nbytes;
            mutex_unlock(&priv->lock);
            return ret;
        }
        seen = priv->next_seq;
        mutex_unlock(&priv->lock);

        /* Nothing wanted: -EAGAIN, or sleep until new records arrive.
         * Never returns 0, which callers would read as end of file. */
        if (filp->f_flags & O_NONBLOCK)
            return -EAGAIN;
        if (wait_event_interruptible(mon_ctx.waitq, READ_ONCE(mon_ctx.seq) != seen))
            return -ERESTARTSYS;
    }
}

static __poll_t sm_poll(struct file *filp, poll_table *wait)
{
    struct sm_priv *priv = filp->private_data;
    __poll_t mask = 0;

    poll_wait(filp, &mon_ctx.waitq, wait);
    if (!priv)
        return EPOLLERR;
    mutex_lock(&priv->lock);
    if (sm_pending(priv))
        mask |= EPOLLIN | EPOLLRDNORM;
    mutex_unlock(&priv->lock);
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

    raw_spin_lock_init(&mon_ctx.lock);
    init_waitqueue_head(&mon_ctx.waitq);
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
