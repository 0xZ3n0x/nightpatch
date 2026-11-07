/*
 * nightpatch - ELDEN RING NIGHTREIGN frame rate unlocker for Linux/Wine.
 *
 * This module is the application half of the split: it owns everything the
 * game requires - the byte patterns, the decoders, which site uses which
 * action, the frame time - and describes itself to the framework (trapkit) with
 * a struct tk_target.  The framework runs the find -> scan -> decode -> arm
 * -> retire -> teardown lifecycle for it; when a breakpoint fires, the
 * framework's #DB handler performs the store the instruction would have
 * performed - with a substituted value - and steps over it.  Not one byte of
 * the game is ever written, so nothing has to be undone: dropping the
 * breakpoints restores stock behaviour exactly.
 *
 * Units, by responsibility:
 *   nr_main.c       module lifecycle, the target description and the frame time
 *   nr_chardev.c    /dev/nightpatch and the four ioctls
 *   nr_sites.c      the site catalogue: patterns, decoders, locate()
 *   nr_softfloat.c  1/fps as IEEE-754 without an FPU
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>

#include "nr_internal.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("the nightpatch authors");
MODULE_DESCRIPTION("Nightreign frame rate unlocker via hardware breakpoints (no game code writes)");
MODULE_VERSION("1.0");

/* Frame time, from nr_set_fps() under nr_lock; READ_ONCE'd in the trap
 * handler through the frametick action's live pointer on every frame. */
s32 nr_target_fps = NR_DEFAULT_FPS;
u32 nr_frame_time_bits;
DEFINE_MUTEX(nr_lock);

/* The application's contribution to trapkit: what to watch for, which sites
 * exist, and how to turn scan matches into resolved sites.  The site table
 * is assigned below so the ABI's NP_SITE_* enum, which nr_sites.c owns the
 * table itself, stays where its users are. */
struct tk_target nr_target;

static const struct tk_site_desc nr_site_descs[NP_SITE_COUNT] = {
    [NP_SITE_FRAMETICK] = {.name = "frametick", .phase = TK_PHASE_REQUIRED},
    [NP_SITE_HERTZ] = {.name = "hertz", .phase = TK_PHASE_REQUIRED},
    [NP_SITE_INTRO_BNE] =
        {.name = "intro/bne", .phase = TK_PHASE_OPTIONAL, .flags = TK_SITE_ONESHOT},
    [NP_SITE_INTRO_FROMSOFT] =
        {.name = "intro/fromsoft", .phase = TK_PHASE_OPTIONAL, .flags = TK_SITE_ONESHOT},
};

static void nr_target_init(void)
{
    nr_target.api_version = TK_API_VERSION;
    nr_target.owner = THIS_MODULE;
    nr_target.image_name = "nightreign.exe";
    nr_target.sites = nr_site_descs;
    nr_target.site_count = NP_SITE_COUNT;
    nr_target.locate = nr_locate;
    nr_target.state = NULL;
}

s32 nr_set_fps(s32 requested_fps)
{
    s32 clamped = nr_clamp_fps(requested_fps);

    mutex_lock(&nr_lock);
    nr_target_fps = clamped;
    /* Read locklessly by the trap handler on every frame. */
    WRITE_ONCE(nr_frame_time_bits, nr_float32_reciprocal((u32)clamped));
    mutex_unlock(&nr_lock);

    return clamped;
}

static int __init nr_init(void)
{
    int ret;

    /* If the integer float arithmetic is wrong the module would write
     * garbage frame times into the game, so refuse to load. */
    ret = nr_softfloat_selftest();
    if (ret)
    {
        return ret;
    }

    /* The clamp is a no-op for the default, so set the bits directly. */
    nr_target_fps = NR_DEFAULT_FPS;
    nr_frame_time_bits = nr_float32_reciprocal(NR_DEFAULT_FPS);

    nr_target_init();

    ret = tk_target_register(&nr_target);
    if (ret)
    {
        pr_err("nightpatch: cannot register the target: %d\n", ret);
        return ret;
    }

    ret = nr_chardev_register();
    if (ret)
    {
        tk_target_unregister(&nr_target);
        pr_err("nightpatch: cannot register /dev/nightpatch: %d\n", ret);
        return ret;
    }

    pr_info("nightpatch: loaded (ABI %d, trapkit API %d)\n", NP_ABI_VERSION, TK_API_VERSION);
    return 0;
}

static void __exit nr_exit(void)
{
    /*
     * First thing, synchronously: stop the watcher, disarm every site and
     * drop the task/mm references.  When this returns no breakpoint exists
     * and no #DB handler can still be inside this module's callback or live
     * pointer, so unloading the text below is safe.  The framework itself
     * cannot have been unloaded underneath us - its module dependency
     * refcount pins it for as long as tk_target_register() registered us.
     */
    tk_target_unregister(&nr_target);

    /* Stop accepting ioctls afterwards; nothing can race the unregister. */
    nr_chardev_unregister();
    pr_info("nightpatch: unloaded\n");
}

module_init(nr_init);
module_exit(nr_exit);
