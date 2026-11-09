# TODO

Ordered by priority. Each task is independent and sized for one agent session. Before you start, read `AGENTS.md`.
When you finish a task, run the done checklist, then delete the task from this file in the same commit.

Every task's acceptance criteria include, without repeating them: `ninja -C build` is warning-free, `ctest --test-dir
build --output-on-failure -j8` passes, changed C++ files pass `clang-format --dry-run --Werror`, and the docs touched
by the change are updated.

## Debug support

Debugging is currently request-scoped: every `debug-registers`, `debug-step`, `debug-watch` and `debug-break` opens
its own QMP connection, starts a GDB stub, performs one operation and tears everything down. These tasks harden that
model first, then extend it. Later tasks that change the wire protocol must follow "Adding a message type" in
`docs/PROTOCOL.md`; append message types and error codes, never renumber them. Live checks are run by the user as
required by `AGENTS.md`; keep the unit suite free of `vmm.so`, `leechcore.so` and root.
