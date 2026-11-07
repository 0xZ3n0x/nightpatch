# Findings from bring-up

Observations on the current game build. They explain several design
decisions that would otherwise look arbitrary.

## Frame-rate sites

- **The primary frametick pattern does not match this build.** The store is
  reached by a short `EB` jump rather than a near `E9` one. The fallback
  pattern matches and decodes to exactly the reference instruction. Both are
  scanned in the same pass, and the primary wins if it is present, so a
  future build that reverts the jump form is still covered.
- **The game-speed patch was dropped.** All three occurrences of the
  reference tool's `timescale` pattern are **cold code** in this version:
  per-task perf breakpoints showed none of them ever execute. The FPS unlock
  needs only frametick and hertz.
- **Hertz runs first during boot,** and again on every focus change
  (alt-tab). The boot run is the one that matters. If the breakpoint is not
  armed by then, the 60 Hz lock holds until the next focus change. This is
  why:
  - the search polls every 300 ms,
  - the scan covers only executable mappings, in a single multi-pattern pass,
    from kernel context,
  - the scan buffer is allocated at enable time, not on the racing path,
  - the intro-skip scans run only *after* the frame-rate sites are armed.
- **No `put_user` faults observed** across sessions. `disable` restores stock
  behaviour, and the frame rate snaps back to 60.

## Intro skip

- The intro LEA search was verified against a synthetic image in userspace.
  The real `np_scan.c` and `np_decode.c` were compiled against shim kernel
  headers and run over hand-built code. A decoy LEA that resolves to the
  movie name but has no following store must be rejected in favour of a
  later one. A store from a register other than RAX must not count, and
  neither must a store beyond the 32-byte lookahead.
- The two name patterns were checked byte for byte against the reference
  Windows mod's AOB strings.
- During boot all four debug registers are in use. Once both logos have been
  skipped, the intro sites are retired and the module is back to two.

## Environment

- The game's executable loads at its PE preferred base `0x140000000`, and
  that base has been stable.
- Tested on x86-64, kernel 6.19, Fedora 43, under Proton.

## Not yet verified

- **Suspend/resume.** `register_wide_hw_breakpoint()` registers on the CPUs
  online at the time. Resume takes the non-boot CPUs offline and back online.
  Check that frametick `hits` keeps climbing after a suspend cycle.
- **The Wine-loader filter on every Proton version.** The search only
  considers processes whose `exe_file` name starts with `wine`. Confirm with
  `ls -l /proc/<game pid>/exe` on new Proton releases.
