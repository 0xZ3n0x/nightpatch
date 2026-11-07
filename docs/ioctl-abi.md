# The ioctl ABI

`src/nightpatch/np_ioctl.h` is the complete interface between `nightctl` and the
module. It is shared verbatim by both.

In the user → kernel direction, **nothing about the game crosses this
boundary**: no address, no instruction, no pid. Userspace can only turn the
watcher on and off, set the frame rate, and read a snapshot. Every decision
about *what* to patch is made in the kernel.

## Device

`/dev/nightpatch`: a misc device, mode `0600` (root only), magic `'N'`.

The device deliberately has no `.release` hook. The watcher is meant to
outlive the process that enabled it, so `nightctl enable` can exit right
away instead of running as a daemon.

## Commands

| Ioctl | Direction / payload | What the kernel does |
|---|---|---|
| `NP_IOC_ENABLE` (`0x01`) | in: `struct np_enable_req` | Clamps the fps, computes the `1/fps` bits, allocates the scan buffer and starts the watcher. If the watcher is already enabled, only the frame rate changes. Fails with `-EINVAL` if `reserved` is non-zero, or `-ENOMEM` if the buffer cannot be allocated. |
| `NP_IOC_DISABLE` (`0x02`) | none | Disarms every site, detaches from the game, frees the buffer and stops polling. A no-op when not enabled. |
| `NP_IOC_SET_FPS` (`0x03`) | in: `__s32` | Clamps the value and publishes the new frame time. It **takes effect live**, with no re-scan or re-arm, because the trap handler reads it on every hit. Works whether or not the watcher is enabled. |
| `NP_IOC_STATUS` (`0x04`) | out: `struct np_status_out` | A self-consistent snapshot taken under `np.lock`. Interruptible (`-ERESTARTSYS`). |

The frame rate is clamped to `[30, 360]` and never rejected, so a nonsense
argument still gives a playable game.

## Structures

```c
struct np_enable_req {
    __s32 target_fps;   /* clamped kernel-side into [30, 360] */
    __u32 reserved;     /* must be zero */
};

struct np_site_status {
    __u64 address;      /* VA of the trap instruction, 0 when not located */
    __u64 hits;         /* trap handler invocations this session */
    __u64 faults;       /* substitutions that could not be written */
    __u8  found;        /* located and accepted by the decoder */
    __u8  fuzzy;        /* matched via the fallback pattern */
    __u8  armed;        /* hardware breakpoint currently installed */
    __u8  retired;      /* one-shot site that fired and released its register */
    __u8  pad[4];
};

struct np_status_out {
    __u32 abi_version;      /* always NP_ABI_VERSION */
    __u32 watcher_state;    /* enum np_watcher_state */
    __s32 game_pid;         /* 0 when not attached */
    __s32 target_fps;
    __u32 frame_time_bits;  /* float32 bits of 1/target_fps */
    __u32 pad;
    __u64 image_base;       /* 0 when not attached */
    __u64 image_size;       /* SizeOfImage */
    struct np_site_status sites[NP_SITE_COUNT];
};
```

The site indices, in order: `NP_SITE_FRAMETICK`, `NP_SITE_HERTZ`,
`NP_SITE_INTRO_BNE`, `NP_SITE_INTRO_FROMSOFT`.

Watcher states: `NP_WATCHER_DISABLED`, `NP_WATCHER_SEARCHING`,
`NP_WATCHER_ARMED`, `NP_WATCHER_FAILED`. See
[architecture.md](architecture.md#watcher-states).

## Versioning

`NP_ABI_VERSION` (currently **3**) is bumped whenever any structure changes
shape. `abi_version` is the first field of the status snapshot, so a
mismatched `nightctl` can detect the difference and say "rebuild" instead of
misreading the layout. `nightctl status` and `nightctl fps` both check it.

The `reserved` field is rejected when non-zero so that it can be given a
meaning later without old callers having passed garbage through it.
