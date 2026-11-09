# TODO

Ordered by priority. Each task is independent and sized for one agent session. Before you start, read `AGENTS.md`.
When you finish a task, run the done checklist, then delete the task from this file in the same commit.

Every task's acceptance criteria include, without repeating them: `ninja -C build` is warning-free, `ctest --test-dir
build --output-on-failure -j8` passes, changed C++ files pass `clang-format --dry-run --Werror`, and the docs touched
by the change are updated.

## Debug support

The tasks below implement the design in `RESEARCH.md` as vertical slices. Later slices depend on the earlier ones.
The live checks are run by the user, as required by `AGENTS.md`; keep the unit suite free of `vmm.so`, `leechcore.so`
and root.

### 1. Add breakpoints and single-step

Only start this after the write-watch path is solid on a live guest.

- Add breakpoint and step messages and provider operations, reusing the RSP client (`Z0`/`Z1`, `s`, `vCont`).
- Decide and document the interaction with the four debug-register slots, existing watchpoints, snapshots and
  migration.
- Add CLI and MCP commands, unit tests with the fake transports, and a live manual procedure.

Acceptance: software or hardware breakpoints and single-step work on a live guest, teardown is unconditional, and
the existing watchpoint path is unchanged.
