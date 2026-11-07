# Architecture

nightpatch is now two kernel modules plus a thin control tool. `trapkit.ko` is
the engine (a git submodule this repo checks out): the reusable engine that watches for a process, scans its PE
image, verifies the instructions it finds, arms hardware breakpoints on
them, and tears everything down when the game exits. `nightpatch.ko` is the
application: the Nightreign patterns, decoders and site table, described to
the engine through its registration API (`third_party/trapkit/src/tk.h`).
The cycle repeats for every launch until it is disabled. `nightctl` only
flips the switch and reads back status.

## End-to-end flow

```
nightctl (user)       nightpatch.ko (application)        trapkit.ko (framework)        game
   |                            |                                |                  |
   |-- ENABLE fps ------------->| clamp fps, 1/fps -> bits       |                  |
   |   (returns immediately)    |-- tk_target_enable -------->  | start the poll   |
   |                            |                                |                  |
   |                            |                                |--> find: Wine + nightreign.exe
   |                            |                                |--> verify MZ/PE, SizeOfImage
   |                            |-- locate(REQUIRED) ------->    |                  |
   |                            |   scan exec VMAs, one pass --->| all patterns at once
   |                            |   re-read + decode each site   |  verify exact encodings
   |                            |   resolve sites as actions     |
   |                            |                                |--> per-CPU exec bps (all or none)
   |                            |                                |
   |                            |-- locate(OPTIONAL) ------->    |                  |
   |                            |   scan data VMAs for names --->|                  |
   |                            |   scan exec VMAs for LEAs ---->|                  |
   |                            |                                |--> per-CPU exec bps (best effort)
   |                            |                                |
   |                            |                                |<== #DB hw breakpoint ==| trap at site VA
   |                            |                                |  filter: current->mm == game_mm
   |                            |                                |  run the site's action  substituted
   |                            |                                |  regs->ip += skip_len   write, no code
   |                            |                                |                         bytes change
   |                            |                                |--> retire spent one-shots
   |                            |                                |
   |-- SET_FPS n -------------->| new frame time, live, no re-arm |
   |-- STATUS ----------------->| snapshot: trapkit + the app's fps   |
   |-- DISABLE ---------------->|-- tk_target_disable -------->  | disarm, detach, free
```

## The steps

1. **Watch.** A delayed work item on `system_long_wq` polls every 300 ms
   while the target is enabled. It finds the game process
   ([site-discovery.md](site-discovery.md#finding-the-game)).
2. **Scan.** One streaming pass over the image's executable mappings looks
   for all frame-rate patterns at once
   ([site-discovery.md](site-discovery.md#the-frame-rate-patterns)).
3. **Decode.** Each match is re-read and checked byte by byte against the
   exact encoding the application's decoders know
   ([site-discovery.md](site-discovery.md#decoding)), and the hit is
   described as a generic action.
4. **Arm.** Both frame-rate sites get per-CPU execute breakpoints, all or
   nothing.
5. **Intro skip.** Only after the frame-rate sites are armed does the
   application look for the two logo-movie references, one site at a time,
   as a best-effort step
   ([site-discovery.md](site-discovery.md#intro-skip)).
6. **On hit.** The #DB handler runs the site's action: a substituted store
   (or stores), a plain skip, or the site's own callback - then steps over
   the instruction ([trap-handler.md](trap-handler.md)). Userspace is never
   in the path.
7. **Retire.** The intro sites fire once per launch. The next monitor poll
   unregisters each one and gives its debug register back. The trap handler
   cannot do this itself, because unregistering sleeps.
8. **Teardown.** When the game exits, or on `disable`, every breakpoint is
   unregistered. Nothing was written to the game's code, so nothing needs
   restoring. The poll then goes back to searching. Unregistering the target
   itself (at module exit) is synchronous: no breakpoint, no poll and no
   in-flight #DB handler remains afterwards
   ([framework.md](framework.md#lifetime-rules)).

## Execution model

Everything except the control surface runs in the kernel:

| # | Phase | Where | What happens |
|---|---|---|---|
| 0 | **Load** (`insmod`) | kernel | `trapkit.ko` first, then `nightpatch.ko`: the application runs the softfloat selftest, registers its target (API version checked by the framework) and `/dev/nightpatch` (0600). |
| 1 | **Enable** | `nightctl enable <fps>` → kernel | The frame time is computed in the application, then `tk_target_enable`: `vmalloc` the 1 MiB scan buffer, queue the poll. **The CLI exits here.** |
| 2 | **Search** | trapkit (workqueue, 300 ms) | `tk_find_game`: `for_each_process` under RCU, skipping non-Wine processes by `exe_file`, with `mmap_read_trylock`/`mmput_async`. PE verification follows with the lock dropped. The winner is kept with `mmgrab` (identity only). |
| 3 | **Scan** | trapkit (workqueue) | `tk_scan_ranges`: one pass over the executable VMAs, all patterns of the phase at once, `cond_resched()` per chunk. The application drives this through `locate()`. |
| 4 | **Decode** | nightpatch (workqueue) | Re-read 17 bytes, verify the exact encoding, map ModRM → `pt_regs` offset, build the action, `tk_site_resolve`. |
| 5 | **Arm** | trapkit (workqueue) | Both REQUIRED sites, all or nothing. |
| 5b | **Intro** | nightpatch + trapkit (workqueue) | Scan the data VMAs for the movie names, scan the executable VMAs for the LEAs that load them, decode and resolve each. The framework arms them one by one. Best effort throughout. |
| 6 | **Trap hit** | trapkit (#DB) | Run the site's action, then step over the instruction. Userspace is never in the path. |
| 7 | **Monitor** | trapkit (workqueue, 2 s) | Retire spent one-shot sites. Liveness comes from `task->exit_state` / `task->mm`. On exit: disarm, drop references, log the session, return to step 2. |
| 8 | **Disable** | `nightctl disable` → kernel | Disarm, detach, free the scan buffer, stop polling. |
| 9 | **Unload** (`rmmod`) | kernel | `rmmod nightpatch` calls `tk_target_unregister()` first (synchronous teardown) then drops `/dev/nightpatch`. `rmmod trapkit` is refused while nightpatch is loaded. |

## Watcher states

```
            enable                 game found,              game exits
 DISABLED ─────────► SEARCHING ───── sites armed ────► ARMED ──────────┐
     ▲                  ▲   │                                           │
     │                  │   └── game found, unpatchable ─► FAILED ──────┤
     │                  │                                    (waits for │
     │                  │                                     exit)     │
     │                  └───────────────────────────────────────────────┘
     └──────────── disable (from any state) ───────────────────────────
```

`FAILED` stays attached to the task on purpose. Without it, an unpatchable
game would be rescanned every 300 ms for as long as it ran.

## Source layout

```
third_party/trapkit/         the engine (git submodule, its own repo:
                             trapkit.ko; API in ./src/tk.h, sources
                             ./src/tk_*.c)

src/nightpatch/              the Nightreign application (nightpatch.ko)
  nr_main.c                  module lifecycle, the target description and
                             the frame time
  nr_chardev.c               /dev/nightpatch and the four ioctls (no policy)
  nr_sites.c                 the site catalogue: byte patterns, decoders,
                             locate(), the actions
  nr_softfloat.c             1/fps as IEEE-754 without an FPU
  np_ioctl.h                 the shared user-facing ABI

src/nightctl.c               the CLI: enable / disable / fps / status
Makefile                     top-level build (both modules + CLI),
                             artifacts in build/
```

The boundary is mechanical and checked: `grep -r '#include' third_party/trapkit/src`
shows no application headers, and the application sees the engine through
`third_party/trapkit/src/tk.h` only.

The Makefile builds the engine inside its submodule first
(`make -C third_party/trapkit module`), stages the application sources into
`build/kmod/`, generates a Kbuild makefile for `nightpatch.o`, and runs the
external-module build with `KBUILD_EXTRA_SYMBOLS` pointing at the engine's
`Module.symvers` - so nightpatch.ko resolves trapkit's symbols with the
right CRCs, and a mismatched pair is refused at load time. That keeps the
application tree free of build artifacts.
