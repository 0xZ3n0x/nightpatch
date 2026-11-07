/*
 * nr_chardev.c - /dev/nightpatch and its four ioctls.
 *
 * User arguments are copied in and out with the framework's target lock
 * released, so a userspace page fault on the caller's buffer can never stall
 * the watcher.  What gets patched where is not userspace's business: no
 * ioctl takes an address or a patch description, only the four switches the
 * ABI v3 defines.
 */
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>

#include "nr_internal.h"

/* The ordinals have stayed aligned since ABI v1; check it stays so, since
 * the snapshot is then copied value for value. */
_Static_assert((int)NP_WATCHER_DISABLED == (int)TK_STATE_DISABLED, "watcher state ordinals diverged from trapkit");
_Static_assert((int)NP_WATCHER_SEARCHING == (int)TK_STATE_SEARCHING, "watcher state ordinals diverged from trapkit");
_Static_assert((int)NP_WATCHER_ARMED == (int)TK_STATE_ARMED, "watcher state ordinals diverged from trapkit");
_Static_assert((int)NP_WATCHER_FAILED == (int)TK_STATE_FAILED, "watcher state ordinals diverged from trapkit");

/*
 * Framework part: a self-consistent snapshot of the watcher and the sites.
 * Application part: the frame rate and target fps, which live behind
 * nr_lock in this module, not in the framework.
 */
static int fill_status(struct np_status_out* out)
{
    struct tk_snapshot snap;
    unsigned int i;
    int ret;

    memset(out, 0, sizeof(*out));
    out->abi_version = NP_ABI_VERSION;

    ret = tk_target_snapshot(&nr_target, &snap);
    if (ret)
    {
        return ret;
    }

    /* Interruptible: a scan in progress holds nr_lock for a little while
     * when the frame rate is being changed. */
    if (mutex_lock_interruptible(&nr_lock))
    {
        return -ERESTARTSYS;
    }
    out->target_fps = READ_ONCE(nr_target_fps);
    out->frame_time_bits = READ_ONCE(nr_frame_time_bits);
    mutex_unlock(&nr_lock);

    out->watcher_state = snap.state;
    out->game_pid = snap.game_pid;
    out->image_base = snap.image_base;
    out->image_size = snap.image_size;

    for (i = 0; i < snap.site_count && i < NP_SITE_COUNT; i++)
    {
        out->sites[i].address = snap.sites[i].address;
        out->sites[i].hits = snap.sites[i].hits;
        out->sites[i].faults = snap.sites[i].faults;
        out->sites[i].found = snap.sites[i].found;
        out->sites[i].fuzzy = snap.sites[i].fuzzy;
        out->sites[i].armed = snap.sites[i].armed;
        out->sites[i].retired = snap.sites[i].retired;
    }
    return 0;
}

static long nr_dev_ioctl(struct file* file, unsigned int cmd, unsigned long arg)
{
    void __user* user_arg = (void __user*)arg;

    /* Reject anything that is not ours before looking at the argument. */
    if (_IOC_TYPE(cmd) != NP_MAGIC)
    {
        return -ENOTTY;
    }

    switch (cmd)
    {
    case NP_IOC_ENABLE: {
        struct np_enable_req req;
        struct tk_snapshot pre;
        s32 applied;
        bool first;
        int ret;

        if (copy_from_user(&req, user_arg, sizeof(req)))
        {
            return -EFAULT;
        }
        /* Rejected now so the field can be given a meaning later without
         * old callers passing garbage through it. */
        if (req.reserved)
        {
            return -EINVAL;
        }

        /* Enable while already enabled means "change the frame rate"; log
         * the start of a watch only when the watcher was idle before.  The
         * frame rate is applied only if the watcher actually is (or gets)
         * running, so a failed enable leaves the previous value alone. */
        first = tk_target_snapshot(&nr_target, &pre) || pre.state == TK_STATE_DISABLED;

        ret = tk_target_enable(&nr_target);
        if (ret == 0)
        {
            applied = nr_set_fps(req.target_fps);
            if (first)
            {
                pr_info("nightpatch: watching for %s at %d fps\n", nr_target.image_name, applied);
            }
        }
        return ret;
    }

    case NP_IOC_DISABLE:
        tk_target_disable(&nr_target);
        return 0;

    case NP_IOC_SET_FPS: {
        s32 requested_fps;

        if (copy_from_user(&requested_fps, user_arg, sizeof(requested_fps)))
        {
            return -EFAULT;
        }
        nr_set_fps(requested_fps);
        return 0;
    }

    case NP_IOC_STATUS: {
        struct np_status_out out;
        int ret = fill_status(&out);

        if (ret)
        {
            return ret;
        }
        return copy_to_user(user_arg, &out, sizeof(out)) ? -EFAULT : 0;
    }

    default:
        return -ENOTTY;
    }
}

static const struct file_operations nr_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = nr_dev_ioctl,
};

/*
 * Mode 0600: only root may open the device.  There is deliberately no
 * .release hook - the watcher is meant to outlive the process that enabled
 * it, so "nightctl enable" can exit immediately instead of running as a
 * daemon.  NP_IOC_DISABLE and rmmod are the ways to stop it.
 */
static struct miscdevice nr_misc_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "nightpatch",
    .fops = &nr_fops,
    .mode = 0600,
};

int nr_chardev_register(void)
{
    return misc_register(&nr_misc_device);
}

void nr_chardev_unregister(void)
{
    misc_deregister(&nr_misc_device);
}
