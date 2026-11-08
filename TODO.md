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

Before starting the watchpoint slice, validate whether `Z2` must be armed per vCPU and whether a debug-stop pause
interacts badly with snapshots, reset or migration, and record the answers under "Checks still owed to a live guest"
in `docs/DEBUGGING.md`.

### 1. Add process-scoped write watchpoints

Implement the experiment from `RESEARCH.md`: catch a write to a linear address, verify it belongs to the requested
process, and resolve the trap PC.

- Add `DebugWatchRequest` (`0x003E`) and the streamed `DebugWatchData` (`0x8061`) / `DebugWatchEnd` (`0x8062`).
  The request carries pid, address, width (1/2/4/8), access (write only in this task), max hits, and a timeout. The
  data frame carries vCPU/thread, `rip`, `rsp`, `cr3`, old and new values, 16 bytes before `rip`, and module/offset,
  nearest-export and optional already-loaded-PDB symbolization. The end frame carries status, hit count, skipped-hit
  count and an error string.
- Add the streamed handler and provider path. Stop the VM, ensure the stub, read `proc-info` for `dtb` and `dtbUser`,
  arm `Z2`, continue, and wait with short polls so `Session::requestAborted`, the request deadline and peer EOF are
  all observed. Add `Session::peerGone()` if the existing check cannot detect a dead client.
- On every stop reply, capture registers before resuming. Mask the low twelve `cr3` bits and compare against both
  process DTBs. Resume and keep watching on a mismatch, counting it as skipped.
- On a match, read the new value, resolve `rip` with the existing module-by-address lookup, and find the nearest
  export. Do not start a PDB download; `PdbSymbolName` may be used only against symbols already loaded.
- Tear down with RAII on success, timeout, cancellation, error and connection death: remove the watchpoint, stop the
  stub if the host owns it, resume the VM if this request paused it, and verify with `query-status`. Refuse a
  concurrent debug operation with `Busy`; enforce the four-watchpoint hardware limit.
- Add the client streaming function, `--client debug-watch` with `--pid`, `--address`, `--width`, `--access`,
  `--max-hits` and `--timeout`, and an MCP tool. The CLI uses one dedicated connection per watch.
- Tests: arm/continue/hit/resolve/teardown with fake QMP, RSP and memory; CR3 mismatch skip; cancellation before and
  after the first data frame; timeout; error-path teardown; peer death; concurrent `Busy`; capability gating.
- Document the manual live procedure in `docs/DEBUGGING.md`: stop, arm, trigger a write, assert the hit, resume and
  verify `query-status`. Record the teardown rule for snapshots, reset and migration.

Acceptance: the user can watch a write in a live Windows process, receive a `DebugWatchData` frame with the trap PC,
CR3, module and nearest export, and afterwards find the VM running, the watch removed and the stub stopped. No guest
memory contents are logged.

### 2. Add address resolution and watch-hit ergonomics

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

### 3. Add breakpoints and single-step

Only start this after the write-watch path is solid on a live guest.

- Add breakpoint and step messages and provider operations, reusing the RSP client (`Z0`/`Z1`, `s`, `vCont`).
- Decide and document the interaction with the four debug-register slots, existing watchpoints, snapshots and
  migration.
- Add CLI and MCP commands, unit tests with the fake transports, and a live manual procedure.

Acceptance: software or hardware breakpoints and single-step work on a live guest, teardown is unconditional, and
the existing watchpoint path is unchanged.
