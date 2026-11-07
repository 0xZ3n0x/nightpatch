# Site discovery

How the module goes from "some process is running" to "these four addresses
are safe to put breakpoints on". All of this runs on the watcher workqueue,
in sleepable context, with `np.lock` held.

## Finding the game

`tk_find_game()` (`tk_process.c`) works in two phases. It has to:
`access_process_vm()` takes the target's `mmap_lock` itself, so headers can
only be read once the VMA walk has released that lock.

**Phase one: collect candidates, under RCU.**

- Walk the task list with `for_each_process`, skipping kernel threads.
- Take an `mm_users` reference with `get_task_mm()`.
- Skip any process whose `mm->exe_file` is not named `wine*`. Under Wine the
  process's real executable is the Wine loader (`wine64-preloader`,
  `wine-preloader`, `wine64` or `wine`). This check is a few pointer loads,
  whereas the VMA walk costs time proportional to every address space on the
  machine. A browser alone has tens of thousands of mappings.
- `mmap_read_trylock()` the mm. A busy lock means the process is skipped for
  this round; the next poll comes 300 ms later. Then look for a file-backed
  VMA whose dentry name is `nightreign.exe` (case-insensitive). The mapping
  at file offset 0 is the image base.
- Release with `mmput_async()`, since nothing here may sleep.
- Collect at most 8 candidates. Several Wine processes can map the image.

**Phase two: verify, with no locks held.**

- Read `MZ`, `e_lfanew` (bounded to `0x40..0x400`), the `PE\0\0` signature
  and `SizeOfImage` (bounded to `0x1000..0x20000000`).
- Under `mmap_read_lock`, split the image's VMAs, clipped to
  `[base, base + SizeOfImage)`, into **executable** ranges (where the code
  sites live) and **readable non-executable** ranges (where the movie names
  live). At most 16 of each.
- The first candidate that verifies and has executable code wins. Its
  `mm_users` reference is traded for `mm_count` (`mmgrab` + `mmput`). From
  then on the mm is only an identity for the trap handler to compare against.

## The scanner

`tk_scan.c` is game-agnostic. It streams memory through a 1 MiB buffer and
tests every still-unmatched pattern against each chunk.

- A pattern is `bytes` + `mask`, where mask `0xFF` pins a byte and `0x00` is
  a wildcard. Every pattern used here pins its first byte, so the inner loop
  is a word-at-a-time `memchr` for that byte, with a full compare only on a
  hit.
- Consecutive chunks overlap by `longest_pattern - 1` bytes, so a match that
  straddles a boundary is still found.
- A short or failed read means a hole. Scanning resumes at the next page
  boundary instead of giving up.
- An optional `verify(match, va, context)` callback gets the final say on a
  byte match. If it rejects one, the search continues one byte further on. The
  callback must be pure: it may only read the bytes it is given, and the
  pattern length is chosen so those bytes include everything it needs.
- The first accepted match wins for each pattern, and the scan returns early
  once every pattern has matched.

## The frame-rate patterns

All three are tested in a single pass over the executable ranges.

| Pattern | Bytes | Meaning |
|---|---|---|
| frametick primary | `C7 ?? ?? ?? 88 88 3C E9` | `mov dword [reg+disp8], 0x3C888889` (1/60 as float32) followed by a near `jmp` |
| frametick fallback | `89 73 ?? C7 ?? ?? ?? ?? ?? ?? EB ?? 89 73` | the same store reached by a short `jmp`, so the `E9` anchor is missing. The store begins 3 bytes into the match. **This is the one the current build matches.** |
| hertz | `C7 45 ?? 3C 00 00 00 C7 45 ?? 01 00 00 00` | `mov [rbp+d1], 60` + `mov [rbp+d2], 1`: the 60 Hz refresh lock |

Patterns are listed primary-first. The first one per site that matches *and*
decodes wins. Both frame-rate sites must resolve, or the game is left alone
(state `FAILED`).

## Decoding

A pattern match alone never reaches a debug register. The framework runs
what the decoders say, so each match is re-read (a 17-byte window, one byte
before the site plus the longest encoding) and checked by the application,
via the framework's `tk_x86_*` helpers:

**frametick**

- the opcode is `C7`, and ModRM has `mod=01`, `reg=0`, and `rm≠100` (no SIB),
  checked by `tk_x86_decode_mov_m32_imm32()`
- the immediate is exactly `0x3C888889`. This matters most for the fallback
  pattern, which leaves the whole immediate as a wildcard.
- `rm` is mapped to an offset in `struct pt_regs` through a fixed table in
  the framework. Only the register *index* comes from game memory; the
  layout mapping stays in trapkit.

**hertz**

- two `C7 45 disp8 imm32` stores, with immediates of exactly 60 and 1

**Leading-prefix check (frame-rate sites only)**

If the byte before a bare match is a REX, `66`, `67`, `F0`, `F2`, `F3` or
segment prefix, the match is rejected, because a prefix would change what
the instruction means. The check is skipped when the pattern itself pinned
the preceding instruction: the frametick fallback starts with
`89 73 disp8`, so the byte before the store is that `disp8`, and
`0x40`–`0x4F` there is an ordinary displacement.

Each match also has to fall inside `[image_base, image_base + SizeOfImage)`.

## Intro skip

The two logo movies are loaded by asset name, stored in the image as
NUL-terminated UTF-16:

| Site | Name | Logo |
|---|---|---|
| `intro/bne` | `05_901_Logo_BNE` | Bandai Namco |
| `intro/fromsoft` | `05_900_Logo_FromSoft` | FromSoftware |

What gets trapped is the `lea rax, [rip+disp32]` that loads the name's
address. Such a LEA only shows what it loads once you know where the string
is, so finding it takes two scans:

1. **Names.** Search the readable non-executable ranges for both names,
   pinned byte for byte. If one is missing there, search the executable ranges
   too. How a PE's sections are split into VMAs is up to the loader.
2. **References.** Search the executable ranges for `48 8D 05 <disp32>`, with
   a verify callback:
   - the displacement must resolve to the name's address, and
   - the byte sequence of a `REX.W 89 /r` store with RAX as its source must
     start somewhere in the 32 bytes after the LEA. The store separates the
     reference that feeds the movie-name field from an incidental mention.

   This store test is a sliding byte window, not an instruction decoder. It
   does not follow instruction boundaries or check that RAX survives until the
   store. That looseness is deliberate: the displacement already pins the
   identity, and these sites write no memory, so a wrong pick can at worst
   leave a logo playing. Tightening it would need the real game's bytes to
   confirm it still matches.

The decoder re-checks the `48 8D 05` encoding and re-resolves the
displacement. It deliberately does **not** apply the leading-prefix test.
The byte before an instruction is the tail of the previous one, and `0x48` is
everywhere in x86-64 code. If the site were not on an instruction boundary,
an execute breakpoint there would simply never fire.

The intro sites are armed **after** the frame-rate sites, one at a time, as a
best-effort step. The hertz site races the game's boot, and nothing here is
worth delaying that for. If no debug register is left (for example, a `gdb`
hardware breakpoint holds one), a logo plays and the frame rate is unaffected.
