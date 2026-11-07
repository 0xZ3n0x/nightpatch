# The trap handler

The breakpoint hit is the one part of nightpatch that *has* to run in the
kernel. Everything else could in principle live in userspace. This could not.

## What happens on a hit

```
game thread executes at va (e.g. C7 43 20 89 88 88 3C = mov [rbx+0x20], 1/60)
  └─ CPU debug register DR0..DR3 matches RIP == va  → #DB exception
       └─ kernel perf bp subsystem → tk_breakpoint_hit (interrupts off, game's context)
            1. filter: current->mm == game_mm && user_mode(regs)?  no → return
            2. atomic64_inc(&site->hits)
            3. the site's action, as resolved at patch time:
               STORE32:   addr = regs_get_register(regs, base) + disp
                          value = live ? READ_ONCE(*live) : imm
                          tk_write_u32_trap(addr, value)   (1..2 stores per site)
               SKIP:      nothing; no memory is touched, so it cannot fault
               failed write → atomic64_inc(&faults), return with RIP untouched
            4. regs->ip += skip_len  (7, 14 or 7; never executed)
            5. kernel has set X86_EFLAGS_RF → no re-trap on resume
game thread resumes at va+len; the substituted value sits in memory
```

| Site | Emulation | Bytes skipped |
|---|---|---|
| frametick | write `float32(1/fps)` to `[reg+disp8]` | 7 |
| hertz | write `0` to `[rbp+d1]` and `[rbp+d2]` | 14 |
| intro | nothing | 7 |

## Why it must be kernel code

- The RIP change has to be **synchronous**. A userspace notification of a
  perf event (`SIGIO`) arrives *after* the original instruction has already
  run.
- The handler runs with interrupts disabled in the trapped thread's context,
  so it must be lock-free: atomics, `READ_ONCE`, register arithmetic and a
  `put_user` with page faults disabled. No allocation, no sleeping, no
  `printk`.

## Why per-CPU breakpoints

`register_wide_hw_breakpoint()` installs one event per CPU, so every thread
of the game is covered whichever CPU it runs on, including threads created
after arming. The flip side is that the breakpoint fires for *any* process
that executes that virtual address. Other Wine processes often load images
at the same preferred base. The `current->mm == game_mm` filter is what keeps
them unaffected: for them the handler returns without touching RIP, and the
instruction runs normally.

## When a write fails

`tk_write_u32_trap()` runs `put_user()` with page faults disabled, because
the handler cannot sleep. If the target page is not present or is
copy-on-write, the write fails. The handler then counts a fault and
**returns without advancing RIP**, so the game's own instruction runs this
once.

This is deliberately the opposite of "skip anyway". A skipped store whose
substitute never landed would leave whatever stale value the field held
before, which for a stack slot is garbage. For hertz, the fallback is safe
even if the first of the two writes succeeded: the stock pair overwrites both
fields with 60 and 1, so the frame never ends up half substituted.

In practice no faults have been observed (see [findings.md](findings.md)).

## Skipping a LEA

The reference Windows mod skips the logos by overwriting each 7-byte
`lea rax, [rip+disp32]` with NOPs. The `mov [mem], rax` that follows then
stores whatever stale value RAX held instead of a movie name.

Advancing RIP past the LEA does **exactly** that, with nothing written. `lea`
sets no flags, and loading its register is its only effect, so a skipped LEA
and seven `0x90` bytes leave identical machine state. RAX is deliberately
*not* substituted: a deterministic NULL would behave differently from the
reference mod, which is known to work. These sites touch no memory, so
unlike the other two they cannot fault.

## `1/fps` without an FPU

The kernel is built with `-mno-sse -mno-mmx -mno-sse2` and may not touch the
FPU in this context, so `nr_softfloat.c` builds the IEEE-754 binary32 bit
pattern with integer arithmetic.

For `2^h <= d < 2^(h+1)`, the reciprocal lies in `(2^-(h+1), 2^-h]`. The
normalised exponent is therefore `-h` for an exact power of two and `-(h+1)`
otherwise. Scaling the numerator by `2^(23 - exponent)` puts the quotient
directly on the 24-bit significand. If rounding carries past the implicit
leading 1, the significand is shifted down and the exponent bumped.

**Rounding.** `DIV_ROUND_CLOSEST_ULL` rounds halves away from zero, whereas
IEEE rounds them to even. The two can only differ on an exact tie. A tie
needs `2^k mod d == d/2`, which forces the odd part of `d` to be 1, so `d`
is a power of two. In that case the division is exact and there is no tie.
The two rounding modes therefore agree for every divisor.

**Verification.** The function matches hardware `1.0f / d` bit for bit for
every `d` in 1..100000, far beyond the [30, 360] the module uses. A selftest
of reference values runs at `insmod`. If it fails, the module refuses to load
rather than write garbage frame times into the game.

The new frame time is published with `WRITE_ONCE` and read with `READ_ONCE`
on every hit. That is why `nightctl fps` takes effect live, with no re-scan
or re-arm.
