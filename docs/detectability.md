# Detectability

> This page documents what nightpatch's design leaves visible, because that
> is part of understanding the technique. It is **not** a guide to hiding
> software from anti-cheat, and the project's
> [proof-of-concept scope](../README.md) applies here in full. The analysis
> below is not a guarantee. Anti-cheat behaviour changes on the server side,
> and using this still carries whatever account risk the game's terms attach
> to modifying it.

On Linux/Proton, EAC cannot load its Windows kernel driver. It runs entirely
in userspace inside the game's Wine process. Every artifact nightpatch
creates was reviewed against what such a process can observe.

## Artifacts

| Artifact | How a userspace process would try to observe it | Result |
|---|---|---|
| Modified game code bytes | integrity hash of code pages | **clean**: zero bytes of code are ever written |
| Debugger / ptrace traces | `TracerPid` in `/proc/self/status`, `PTRACE_ATTACH` side effects, `int3` bytes | **clean**: no ptrace, no software breakpoints |
| Debug registers (DR0–DR3, DR7) | `mov rax, dr7` | **unreadable**: a privileged instruction, #GP from any user thread |
| Perf breakpoint events | enumerate perf events | **no interface**: userspace cannot enumerate kernel or per-CPU perf events |
| Reads of game memory | page-state changes, open handles, `/proc/<pid>/io` of a scanning process | **no trace**: reads come from `access_process_vm` in kernel context, so there is no scanning process and no handle into the game |
| Substituted values in game data | poll the frametick field and compare it with 1/60 | **indistinguishable** from any other FPS unlock: the write happens in the game's own context, at the game's own address |
| Skipped intro LEAs | anything | **nothing to see**: no memory is written; a register just keeps the value it already had, exactly as with a NOP'd instruction |
| RIP skipping / RF flag / DR6 | self-observation | **transparent**: a thread cannot observe that its own `mov` was skipped; DR6 is privileged |
| #DB handler overhead | timing analysis | sub-µs per frame, which is noise. The pacing change is the unlock itself and looks like a genuine high-refresh setup. |
| `dmesg` lines | read the kernel log | **restricted** by `kernel.dmesg_restrict` (root only) |
| Module parameters | `/sys/module/nightpatch/parameters/` | **empty**: the module has none |

## What remains visible

| Surface | Who can see it | What it reveals |
|---|---|---|
| `/proc/modules`, `/sys/module/nightpatch/`, the `/dev/nightpatch` node | any process (mode 0600 prevents opening, not listing) | that a root-owned module named "nightpatch" exists |
| `nightctl` process entry | any process listing | only while a command runs, for milliseconds; there is no long-lived process |
| `/proc/sys/kernel/tainted` (`O` + `E`) | world-readable | an out-of-tree, unsigned module is loaded. This is true of *any* such module and carries no name. |

So none of the channels above shows more than "a root-owned module with this
name exists". The supporting evidence is this analysis plus one test session
with EAC active and no detection, and nothing more.

## A note on taint

Earlier versions set `MODULE_INFO(intree, "Y")`. That suppressed the `O`
taint flag, so `/sys/module/nightpatch/taint` read `E` where every other
out-of-tree module reads `OE`. This made the module *more* anomalous, not
less, and it misled the kernel's own debugging machinery. It was removed.
The module now reports itself honestly as out-of-tree.
