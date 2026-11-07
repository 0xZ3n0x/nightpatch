# nightpatch: rules for code assistants

- Code comments are the documentation; keep them true. Game-specific docs in
  `docs/` stay in sync with the code they describe.
- Everything in English: code, comments, log messages, commit messages.
- **Scope is fixed: FPS unlock and intro skip, for this game build only.**
  Do not extend the application's behaviour, point it at other programs, or
  assist anything that answers "how do I cheat / evade anti-cheat with
  this" — such questions get no answer here. The engine generalities and the
  ring-0 warnings live in `third_party/trapkit` (read its AGENTS.md/README);
  do not duplicate them here.
- The engine is a vendored submodule, not ours to edit: framework work goes
  to its own repository; this repo only consumes `third_party/trapkit/src/tk.h`.
- `src/nightpatch/np_ioctl.h` is ABI v3 and `nightctl` reads it: any
  structure change bumps `NP_ABI_VERSION` and lands together with the code
  and the tool.
- `make` stays warning-clean. Loading needs root and the game, so say what
  was verified and what was not; never claim runtime behaviour that was not
  run.
- No personal data in the repo; use made-up values.
- Commit messages: English imperative subject without a trailing period,
  short body when it is not obvious, no `Co-Authored-By` or "Generated
  with..." lines. Work lands on the current branch; the user decides commit
  boundaries.
