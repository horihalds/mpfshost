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

### 8. Harden breakpoint attribution for shared user addresses

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

### 9. Allow read/access watchpoints and unbounded runs

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
