# nightpatch

Removes the 60 FPS cap of *ELDEN RING NIGHTREIGN* and skips its two startup
logo movies, on Linux under Wine/Proton, **without writing a byte to the
game's code**: hardware execute breakpoints trap a few instructions and a
kernel handler substitutes their effect instead. It is an application built
on the trapkit engine, which ships here as a submodule; supporting another
game in the future means a new application module, not an engine change.

`nightctl` is the control tool: one ioctl, no daemon.

> [!WARNING]
> **This application does exactly two things and nothing else**: frame-rate
> cap removal and intro-skip, for this game build only. The kernel-level
> machinery and its ring-0 risks live in the trapkit engine
> ([third_party/trapkit](third_party/trapkit)) — read the warning in its
> README first.
>
> Running it may break the game's terms of service. Every consequence is
> yours alone — up to and including an account ban, **no liability is
> accepted**. Questions about adapting this code to cheat, read other
> players or evade anti-cheat **will not be answered**. Not affiliated with
> or endorsed by FromSoftware or Bandai Namco.

## Build and run

```sh
git submodule update --init             # engine: third_party/trapkit
make                                    # third_party/trapkit/build/kmod/trapkit.ko,
                                        # build/kmod/nightpatch.ko, build/nightctl

sudo make load                          # insmod engine, then application
sudo build/nightctl enable 144          # watch for the game at 144 FPS
# ...start the game from Steam...
sudo build/nightctl status              # arm the frame rate race before launch if you can
sudo make unload                        # disable, then rmmod nightpatch, trapkit
```

Needs: kernel headers for the running kernel, `gcc`, `make`, root, and
Wine/Proton running the game. With Secure Boot, sign the module with an
enrolled MOK key or unload will refuse.

**Frame rate is clamped to 30–360.** The watcher keeps running in the kernel
(after `nightctl` exits) until `disable` or `rmmod`; patches each launch,
restarts included. `fps <n>` changes the target live, no re-scan.

## Reading `status`

```
state      : armed
target fps : 144
frame time : 0.006944444 s (bits 0x3be38e39)
  frametick     : 0x1408a1c5e [fuzzy] armed  hits 51840  faults 0
  hertz         : 0x140f3d2a0       armed  hits 1     faults 0
  intro/bne     : 0x1412b7c40       retired  hits 1   faults 0
  intro/fromsoft: 0x1412b7d18       retired  hits 1   faults 0
```

- `frametick`: `armed`, `hits` climbing while you play. `[fuzzy]` = the
  fallback pattern matched, normal for this build.
- `hertz`: `armed`, `hits` ≥ 1 (boot + every focus change).
- `intro/*`: `retired` after one hit per launch; they give their debug
  register back.
- `faults` counts substitutions that could not be written; the stock
  instruction ran instead, so it costs one frame, never a crash.
- `searching` = waiting for the game; `game found but not patchable` =
  usually a game update (see `dmesg` for the site).

Debug registers: all four during boot, two afterwards. Something else
holding one (gdb, perf) costs intro sites; `dmesg` names the skipped logo.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `Key was rejected by service` | Secure Boot/lockdown: sign with an enrolled MOK key. |
| `Invalid module format` | Kernel changed: `make clean && make`. |
| `No such file or directory` on `/dev/nightpatch` | Module not loaded. |
| `Permission denied` | Run `nightctl` with sudo (device is 0600). |
| `softfloat selftest failed` in dmesg | Do not load; report it (compiler + kernel version). |
| `module speaks ABI X, nightctl speaks Y` | Mixed builds: rebuild all, rmmod both, insmod again. |
| Stuck `searching` while the game runs | The game's exe must start with `wine` and map `nightreign.exe` (`grep -i nightreign /proc/<pid>/maps`). |
| Capped at 60 after boot | `enable` was too late for the boot-time `hertz` run; alt-tab once or enable before launch. |
| A logo still plays | Another tool holds a debug register, or a name/reference was not found; `dmesg` says which. |

## Notes

- `./scripts/verify.sh` automates the verification plan (static checks,
  load-order denials, the armed wait, a load/unload loop with leak and oops
  scans, session summary, clean unload).
- Logs: `sudo dmesg -w | grep -E 'nightpatch|trapkit'` — detection, arming,
  retirement, and a per-session hit/fault summary on game exit. Engine
  messages carry the `trapkit:` prefix, application messages `nightpatch:`.
- Upgrading: `rmmod nightpatch && rmmod trapkit`, rebuild, load again.
  `nightctl` and the module refuse mismatched ABI/API.
- The engine's API and internals are documented in its code comments
  (`third_party/trapkit/src/tk.h` first); game-specific details:
  [docs/architecture.md](docs/architecture.md),
  [docs/site-discovery.md](docs/site-discovery.md),
  [docs/trap-handler.md](docs/trap-handler.md),
  [docs/ioctl-abi.md](docs/ioctl-abi.md),
  [docs/detectability.md](docs/detectability.md),
  [docs/findings.md](docs/findings.md).

The module is GPL (`MODULE_LICENSE("GPL")`).
