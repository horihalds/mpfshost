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

### 1. Add address resolution and watch-hit ergonomics

Make a hit directly usable without several client round trips.

- Add a client-side `resolveAddress(pid, address)` helper that combines `moduleByAddress`, nearest export and an
  optional already-loaded PDB symbol. Expose it as an MCP tool and a CLI command; keep it out of the wire protocol
  unless the composition proves too chatty.
- Add an optional conservative x86-64 decoder for the bytes before `rip` so a write watchpoint can report the
  instruction that actually performed the store rather than only the trap-after PC. If the decoder is deferred, make
  sure `bytesBefore` is always present and documented.
- Optionally unwind the stack from the hit's `rsp` by reading return addresses through MemProcFS and symbolizing
  them. Keep this additive and bounded (`kMaxCallstackFrames`).
- Tests: module/export/PDB precedence, address outside every module, malformed or truncated bytes, and decoder edge
  cases if the decoder lands.

Acceptance: one command or tool call turns an absolute RIP into module, offset, nearest export and optional PDB
symbol, and the watch output identifies the store when the decoder is available.

### 2. Keep a write watchpoint armed across guest context switches

Live validation showed that `Z2` traps a write only in a short window after `vCont;c`; later writes reach the watched
buffer without producing a stop. Determine whether Windows rewrites DR7 on each thread switch and whether QEMU/KVM
re-applies it. Consider re-arming after every stop, stopping only the writing vCPU, or a different trap mechanism.
Add a live test that catches a write several seconds after arming.

Acceptance: a host `debug-watch` returns a `DebugWatchData` frame for a write that happens well after the guest has
resumed, then removes the watch and leaves the guest running.

### 3. Add breakpoints and single-step

Only start this after the write-watch path is solid on a live guest.

- Add breakpoint and step messages and provider operations, reusing the RSP client (`Z0`/`Z1`, `s`, `vCont`).
- Decide and document the interaction with the four debug-register slots, existing watchpoints, snapshots and
  migration.
- Add CLI and MCP commands, unit tests with the fake transports, and a live manual procedure.

Acceptance: software or hardware breakpoints and single-step work on a live guest, teardown is unconditional, and
the existing watchpoint path is unchanged.
