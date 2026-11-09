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

### 1. Tear down debug state when the host exits

A host exit or crash currently leaves a paused guest, a live GDB stub and armed debug registers behind, while
`_hostPaused`, `_stubActive` and `_watchCount` vanish with the process.

- Add `DebugController::shutdown()` that best-effort connects to QMP, removes any breakpoint/watchpoint the host
  armed, stops the stub only when `_stubActive` is set, removes the stub socket file, and resumes the guest only when
  `_hostPaused` is set. Never stop a stub the host did not start.
- Call it from `VmmHandle::~VmmHandle` before `VMMDLL_Close`, and keep it idempotent so the per-request teardown paths
  and the destructor can both run.
- Make `debug-status` distinguish "the host owns a stub" from "some foreign stub is active" in its text output.
- Add a `debug-detach <vm>` client command and MCP tool that stops a foreign stub only on explicit request, after
  confirming with `info chardev` and `query-status`, and never resumes a guest it did not pause.
- Tests with the fake QMP and RSP transports: shutdown stops an owned stub, removes an armed watchpoint, resumes a
  host-paused guest, does nothing for a foreign stub, and is safe to call twice.
- Document the shutdown and stale-stub recovery procedure in `docs/DEBUGGING.md`.

Acceptance: stopping the host while a guest is paused leaves the guest running and the stub gone; a killed host can
be recovered with `debug-detach` instead of a manual HMP command.

### 2. Keep one QMP connection for the debug controller

QEMU accepts only one QMP client, so a running watch holds the channel for its whole duration and `debug-status` or
`debug-stop` from another request either returns `Busy` or cannot connect. The host also reconnects for every
single-frame request.

- Open the QMP connection on first use and keep it in `DebugController`, reconnecting when the transport reports EOF
  or a command fails. Guard commands with a QMP mutex rather than holding the controller-wide operation mutex.
- Split the locking: an exclusive operation mutex for watch/break/step/registers, and a separate state mutex for
  `state()` and `control()` so they can run while a watch or breakpoint is in flight. QMP commands are serialized,
  not the whole request.
- Route every QMP call through the session's request deadline instead of the fixed `kQmpTimeout`; keep a bounded
  default for calls made outside a request (teardown, shutdown). Surface a QMP connect failure as
  `CapabilityUnavailable` when the socket is absent and `Internal` only for a real command error.
- Tests: a watch in progress still answers `debug-status` and `debug-stop`; a dropped QMP connection is re-established
  on the next call; a command failure does not poison the cached connection; deadline expiry cancels a pending QMP
  command.

Acceptance: during a live watch, `debug-status` reports the real run state and a second client can pause or resume
the guest.

### 3. Reject guests whose architecture the debugger cannot read

The register layout, `rip`/`cr3` lookups and `x86_decoder` assume x86-64. An aarch64 or 32-bit guest fails late with
"the GDB stub does not expose rip" or, worse, reads the wrong registers.

- Detect the architecture from QMP `query-cpus-fast` (`target`/`qom-type`) and, as a fallback, from the target XML
  register names. Treat only `x86_64` as supported for now.
- Return `CapabilityUnavailable` with a clear message before starting a stub for registers, watch, break or step.
  `debug-status` stays available because it only needs QMP.
- Add the detected architecture to `DebugStatus` as an additive field or, if the payload layout must stay unchanged,
  report it through the existing error text and document the limitation.
- Tests: an aarch64 target description and an unknown architecture are rejected; x86-64 still passes; status remains
  usable for an unsupported guest.
- Document the x86-64-only restriction and what a future generic register form would require in `docs/PROTOCOL.md`
  and `docs/DEBUGGING.md`.

Acceptance: an unsupported guest gets one clear error instead of a stub start or a misleading register failure.

### 4. Add an explicit debug attach and detach

Breakpoints, watchpoints and single-step are usable only as isolated one-shot requests. Introduce the ownership
model before making breakpoints persistent.

- Add `DebugAttachRequest`/`DebugAttachResponse` and `DebugDetachRequest`/`DebugDetachResponse` as new message types.
  Attach starts the stub (only when the guest is paused or the attach itself pauses it), records the owning session
  and returns the run state, stub mode and architecture. Detach removes every host-armed breakpoint and watchpoint,
  stops the stub, resumes the guest only if attach/its operations paused it, and clears the bookkeeping.
- Serialize attaches: a second attach is `Busy`, and a stale stub reported by `info chardev` is `InvalidState` with
  the `debug-detach` hint.
- Detach on peer disconnect, cancellation, host shutdown and `VmmHandle` destruction.
- Add the client functions, CLI commands and MCP tools; mark attach and detach destructive.
- Tests: attach/detach lifecycle with fakes, detach on peer death, a second attach returns `Busy`, a foreign stub is
  refused, and the guest returns to its pre-attach state.
- Document the session model in `docs/DEBUGGING.md`.

Acceptance: two clients cannot own the debug channel at once, and `debug-detach` always restores the pre-attach run
state and stub ownership.

### 5. Make breakpoints and watchpoints persistent for a debug session

Once a session exists, a caller should be able to arm several breakpoints/watchpoints, continue, and receive hits
over multiple requests instead of re-arming for every hit.

- Add `DebugBreakpointArmRequest`/`DebugBreakpointArmResponse` (with an id), `DebugBreakpointRemoveRequest`,
  `DebugBreakpointListRequest`/`Response`, and the equivalent watch set. Keep the existing one-shot messages for
  compatibility.
- Allow at most four hardware slots, shared by hardware breakpoints and watchpoints; reject arming a fifth with
  `InvalidState`. Software breakpoints do not consume a slot but must respect the `Z0` address-space limitation.
- Add `DebugWaitRequest` (timeout, max hits) that returns the next stop for any armed breakpoint/watchpoint, including
  the hit's id, registers and symbolization. Add `DebugContinueRequest` that resumes without disarming.
- Remove every armed resource on detach, host shutdown, snapshot/reset detection and session death.
- Add the client streaming function, CLI commands and MCP tools. Keep the one-shot commands working.
- Tests: arm/continue/hit/remove/list, slot exhaustion, a watch and breakpoint armed together, cancellation during
  wait, teardown on detach, and ids that stay stable while other resources are removed.
- Document the new messages in `docs/PROTOCOL.md` and the workflow in `docs/DEBUGGING.md`.

Acceptance: a caller can arm several breakpoints and watchpoints, continue the guest, receive hits over repeated
waits, remove individual resources and detach with nothing left armed.

### 6. Add debug thread discovery

`--vcpu` is the stub's thread-list index, and callers have no way to see which guest thread or process a register
record belongs to.

- Add `DebugThreadsRequest`/`DebugThreadsResponse`: for each stub thread, the vCPU index, GDB thread id, current
  `rip`, `rsp`, `cr3` and, when the CR3 matches a known process, the pid and guest tid. Reuse the existing stub
  startup and register reads, and stop the stub afterwards when no session is attached.
- Add `--client debug-threads` and an MCP tool, and a `--thread` selector to `debug-step` in addition to `--vcpu`.
  Report the thread id in every hit record, not only the index.
- Tests: fake stub with several threads, pid/tid resolution for matching and non-matching CR3, invalid thread
  selection, and no guest-memory logging.
- Document the index-versus-thread distinction in `docs/DEBUGGING.md` and the request in `docs/PROTOCOL.md`.

Acceptance: a caller can list every vCPU with its current context, resolve the target process threads, and select a
specific thread for stepping.

### 7. Accept symbolic addresses in breakpoint and watch requests

`debug-break` and `debug-watch` take raw hex only, although the host already resolves modules, exports and PDB
symbols.

- Extend `DebugBreakpointRequest` and `DebugWatchRequest` additively with an optional module and symbol/export name
  pair. When present, the host resolves them with the same inspector used for hit symbolization and reports the
  resolved address in the response. Reject an ambiguous or missing symbol with `NotFound`, not `Internal`.
- Support `module!symbol`, `module+offset` and process-relative forms in the CLI and MCP layers, reusing the existing
  `resolve`/`symbol` helpers instead of duplicating parsing.
- Add the resolved address and symbol to the CLI output.
- Tests: export and PDB symbol resolution, a missing module, a missing symbol, an ordinal-only export, and a raw
  address that still works unchanged.
- Document the accepted forms in `README.md` and `docs/PROTOCOL.md`.

Acceptance: a user can break or watch `nt!HalProcessorIdle+0xf` or `guest-probe.exe!GuestProbeMarker` without a
separate address lookup.

### 8. Add disassembly and backtrace output

Watch hits currently render one decoded store instruction; there is no way to inspect code around a stop or walk
the stack.

- Add `DebugDisassembleRequest`/`DebugDisassembleResponse`: read up to a bounded byte count through the inspector at
  a virtual address and return decoded instructions with address, bytes and Intel-syntax text. Extend
  `src/core/debug/x86_decoder.cpp` with a small length decoder rather than shelling out; unknown encodings become
  `.byte` lines instead of guesses.
- Add `debug-disassemble <vm> --pid <pid> --address <hex> [--count <n>]` and an MCP tool.
- Add a backtrace command that reads `rip`, `rsp` and `rbp` and walks frame pointers for the selected thread, bounded
  by a frame and depth limit, with each address symbolized through the existing module/export/PDB path. Do not guess
  past a corrupt frame.
- Tests: decoder length/rendering cases including prefixes and unknown bytes, truncation at the requested count,
  backtrace termination on a null/bad frame pointer, and symbolization failures that leave the address raw.
- Document both commands in `README.md` and `docs/PROTOCOL.md`.

Acceptance: a stopped thread can be disassembled around its PC and its frame-pointer chain printed with module and
symbol names, with no unbounded reads.

### 9. Harden breakpoint attribution for shared user addresses

A hardware breakpoint on a hot system-DLL address traps in every process that maps it. The host currently accepts a
hit when the stop PC matches and the CR3 or module map agrees, so a shared address can be attributed to the wrong
process.

- For a hardware breakpoint, require the stop thread's CR3 to be the target process DTB or `dtbUser` before reporting
  a hit. When a shadow page table defeats that check, mark the hit as unverified in the response instead of silently
  accepting it, and add a response flag for that state.
- Keep the existing PC-equality requirement and the current watch-address rule. Do not weaken either to reduce
  `skippedCount`.
- For software breakpoints on user-mode addresses, select a vCPU that is in the target process before arming when one
  is identifiable, and otherwise return `InvalidState` with a clear explanation instead of a raw `E22`.
- Record the outcome of a live run on `ntdll!RtlEnterCriticalSection` in `docs/DEBUGGING.md`, then close the
  corresponding "Checks still owed to a live guest" item.
- Tests: a matching-PC stop from another process is rejected or flagged, a shadow-CR3 hit in a target module is
  accepted and flagged as appropriate, and a software user-mode breakpoint reports the new state.

Acceptance: a shared-address breakpoint no longer reports another process's hit as the target's without saying so.

### 10. Allow read/access watchpoints and unbounded runs

`DebugWatchAccess` has only `Write`, and `maxHits` defaults to 1 with a hard cap of 64, which makes a watch a
bounded probe rather than a usable tool.

- Add `Read` and `Access` values to `DebugWatchAccess` and map them to the stub's read/access watchpoint commands
  where the architecture supports them. Reject an unsupported access mode with `CapabilityUnavailable` after
  querying the stub, not a timeout.
- Change the hit limit so `0` means "run until cancelled or timed out", raise the cap, and stream a heartbeat or
  status frame so a quiet watch does not appear hung. Keep the existing timeout cap.
- Report the armed access mode in `DebugStatus` and the watch end frame.
- Tests: read/access packet formatting, unsupported access rejection, unlimited-run cancellation, heartbeat emission
  and timeout accounting.
- Document the semantics and the QEMU hardware limits in `docs/PROTOCOL.md` and `docs/DEBUGGING.md`.

Acceptance: a caller can place a read or access watch, run it until cancellation, and see the guest restored.
