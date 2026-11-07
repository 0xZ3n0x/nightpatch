/*
 * nr_internal.h - declarations shared between nightpatch's application units.
 *
 * The game-specific half of the project: patterns, decoders, the target
 * description, frame time, and /dev/nightpatch.  The framework behind it is
 * reached only through tk.h.
 */
#ifndef NR_INTERNAL_H
#define NR_INTERNAL_H

#include <linux/mutex.h>
#include <linux/types.h>

#include "tk.h"
#include "np_ioctl.h"

struct tk_site_snapshot;

/* Frame rate assumed until userspace says otherwise: the game's own cap. */
#define NR_DEFAULT_FPS 60

/* Accepted frame rate range.  Values outside it are clamped, never rejected,
 * so a nonsense ioctl argument still produces a playable game. */
#define NR_FPS_MIN 30
#define NR_FPS_MAX 360

/* The single target this application serves. */
extern struct tk_target nr_target;

/*
 * Application state behind NP_IOC_SET_FPS / STATUS: the target frame rate in
 * effect and the IEEE-754 bits of 1/target_fps, updated by nr_set_fps() under
 * nr_lock, read locklessly in snapshot-status and (via the action's live
 * pointer) by the trap handler on every frame.
 */
extern s32 nr_target_fps;
extern u32 nr_frame_time_bits;
extern struct mutex nr_lock;

/* Set the frame rate (clamped).  Returns the value actually applied. */
s32 nr_set_fps(s32 requested_fps);

/* /dev/nightpatch. */
int nr_chardev_register(void);
void nr_chardev_unregister(void);

/*
 * The target's locate() callback: resolve the sites of one phase against
 * *img, printing the usual diagnostics.  Called with the framework's
 * per-target lock held; sleepable.
 */
int nr_locate(struct tk_target* t, const struct tk_image* img, enum tk_phase phase);

/* 1/fps and its integer-only selftest (see nr_softfloat.c). */
s32 nr_clamp_fps(s32 requested_fps);
u32 nr_float32_reciprocal(u32 divisor);
int nr_softfloat_selftest(void);

#endif /* NR_INTERNAL_H */
