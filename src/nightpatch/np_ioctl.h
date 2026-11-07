/*
 * np_ioctl.h - the complete user-facing ABI of nightpatch.
 *
 * The module is self-contained: it finds the game, scans and decodes its
 * image, installs the hardware breakpoints and tears everything down again
 * on its own.  Userspace only turns the watcher on and off, adjusts the
 * target frame rate, and reads back a status snapshot.  Nothing about the
 * game - no address, no instruction, no pid - ever crosses this boundary
 * in the user -> kernel direction.
 */
#ifndef NP_IOCTL_H
#define NP_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Magic byte every nightpatch ioctl must carry. */
#define NP_MAGIC 'N'

/*
 * Bumped whenever any structure below changes shape.  NP_IOC_STATUS reports
 * it first so a stale nightctl can say "rebuild me" instead of misreading a
 * snapshot laid out differently than it expects.
 */
#define NP_ABI_VERSION 3

/*
 * Patch sites inside the game image.
 *
 * The first two unlock the frame rate and stay armed for the whole session.
 * The intro sites each skip one startup logo movie, fire exactly once during
 * boot, and are retired straight afterwards so their debug register goes back
 * to the machine - see the retired flag in struct np_site_status.
 */
enum
{
    NP_SITE_FRAMETICK = 0,  /* per-frame store of the frame time: mov [reg+disp8], 1/60 */
    NP_SITE_HERTZ,          /* refresh lock: two mov [rbp+disp8], imm32; boot + every focus change */
    NP_SITE_INTRO_BNE,      /* lea rax, ["05_901_Logo_BNE"] - Bandai Namco logo movie */
    NP_SITE_INTRO_FROMSOFT, /* lea rax, ["05_900_Logo_FromSoft"] - FromSoftware logo movie */
    NP_SITE_COUNT
};

/* Watcher lifecycle, reported by NP_IOC_STATUS. */
enum np_watcher_state
{
    NP_WATCHER_DISABLED = 0, /* idle, nothing scheduled */
    NP_WATCHER_SEARCHING,    /* polling the task list for the game */
    NP_WATCHER_ARMED,        /* attached, breakpoints installed */
    NP_WATCHER_FAILED,       /* game found but unpatchable; waiting for it to exit */
};

/* NP_IOC_ENABLE argument. */
struct np_enable_req
{
    __s32 target_fps; /* clamped kernel-side into [30, 360] */
    __u32 reserved;   /* must be zero */
};

/* Per-site half of the status snapshot. */
struct np_site_status
{
    __u64 address; /* VA of the trap instruction, 0 when not located */
    __u64 hits;    /* trap handler invocations */
    __u64 faults;  /* substitutions that could not be written */
    __u8 found;    /* located by the scanner and accepted by the decoder */
    __u8 fuzzy;    /* matched via the fallback pattern */
    __u8 armed;    /* hardware breakpoint currently installed */
    __u8 retired;  /* one-shot site that has fired and released its debug register */
    __u8 pad[4];
};

/* NP_IOC_STATUS result: the module's entire observable state. */
struct np_status_out
{
    __u32 abi_version;     /* always NP_ABI_VERSION */
    __u32 watcher_state;   /* enum np_watcher_state */
    __s32 game_pid;        /* 0 when not attached */
    __s32 target_fps;      /* clamped frame rate in effect */
    __u32 frame_time_bits; /* float32 bits of 1/target_fps, computed in-kernel */
    __u32 pad;
    __u64 image_base; /* nightreign.exe load address, 0 when not attached */
    __u64 image_size; /* SizeOfImage from the PE header */
    struct np_site_status sites[NP_SITE_COUNT];
};

#define NP_IOC_ENABLE  _IOW(NP_MAGIC, 0x01, struct np_enable_req)
#define NP_IOC_DISABLE _IO(NP_MAGIC, 0x02)
#define NP_IOC_SET_FPS _IOW(NP_MAGIC, 0x03, __s32)
#define NP_IOC_STATUS  _IOR(NP_MAGIC, 0x04, struct np_status_out)

#endif /* NP_IOCTL_H */
