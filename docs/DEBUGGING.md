# Debugging a guest

This document records how mpfshost reaches QEMU's debug channels and the manual checks that need a live guest.
Unit tests cover the protocol; anything that touches QMP, HMP or the GDB stub is verified by hand.

## Control plane

Each VM target is a QEMU pid paired with a QMP Unix socket by the target scan (`list-targets` prints both). The host
connects to that socket for every debug request, completes QMP's `qmp_capabilities` handshake, and closes the
connection again.

| Need | Channel | Command |
|------|---------|---------|
| Run state | QMP | `query-status` |
| vCPU count | QMP | `query-cpus-fast` |
| Pause / resume | QMP | `stop` / `cont` |
| GDB stub lifecycle | HMP through QMP | `human-monitor-command` with `gdbserver ...` |
| Registers, breakpoints, watchpoints, stepping | GDB remote serial protocol | see later slices |

`gdbserver` is **HMP-only** in the QEMU versions tested; there is no QMP-native equivalent. The host therefore has to
start and stop a stub through `human-monitor-command`. `debug-registers`, `debug-watch`, `debug-break` and `debug-step`
start a stub; the host records that it owns it and stops it during teardown.

The QMP socket is root-equivalent over the guest: anyone who can connect can reset it, read all RAM, inject input and
hotplug devices. The host's Unix-socket access policy is the gate. Do not expose the QMP socket to broader groups than
the host socket.

## Run states

`debug-status` reports the wire enum derived from QEMU's `query-status` string:

| Wire value | QMP status |
|------------|------------|
| `running` | `running` |
| `paused` | `paused` |
| `shutdown` | `shutdown` |
| `suspended` | `suspended` |
| `guest-panicked` | `guest-panicked` |
| `other` | any other QMP state (`debug`, `inmigrate`, recovery states, …) |
| `unknown` | no state was read |

`hostPaused` means **this** host process performed the pause with `debug-stop` and has not continued it. `debug-stop`
on an already-paused guest is a no-op; `debug-continue` resends `cont` whenever the guest is not running, even if
another client paused it.

A guest stopped by an attached GDB stub reports QMP `debug` while no request is running, which maps to `other` rather
than `paused`. It is still stopped and `debug-continue` resumes it normally. A QMP `stop` does not turn `debug` back
into `paused`; QEMU leaves that state on `cont`.

## Manual check: state, stop and continue

1. Start the host (root) and confirm the target is visible:

   ```sh
   ./build/mpfshost --client list-targets
   ```

2. With the guest running:

   ```sh
   ./build/mpfshost --client debug-status <vm>
   ```

   Expect `run-state=running`, `stub-active=no`, `stub-owned=no`, `foreign-stub=no`, `host-paused=no`,
   `vcpus=<n>` matching the QEMU command line, and `watches=0`.

3. Pause it:

   ```sh
   ./build/mpfshost --client debug-stop <vm>
   ```

   Expect `run-state=paused` and `host-paused=yes`. The guest desktop must stop responding until step 5.

4. Query again and confirm the state is still `paused` and `host-paused=yes` (the pause is not a transient).

5. Resume it:

   ```sh
   ./build/mpfshost --client debug-continue <vm>
   ```

   Expect `run-state=running` and `host-paused=no`.

6. Repeat step 2. If the socket is unavailable or the host is not permitted to connect, the client reports an
   `Internal` failure with the QMP error text.

Run the same sequence through MCP (`debug_status`, `debug_stop`, `debug_continue`) once, to confirm the destructive
annotations and result shape.

## Validation record

On 2026-11-09 against the target `windows` (12 vCPUs):

- `debug-status` reported `running`, `host-paused=no`, `stub-active=no`, `stub-owned=no`, `foreign-stub=no`,
  `watches=0`.
- `debug-stop` reported `paused`/`host-paused=yes`; a second query two seconds later was still `paused` and still
  `host-paused=yes`, so the pause persisted.
- A second `debug-stop` was a no-op and kept `host-paused=yes`. `debug-continue` while already running was a no-op.
- `debug-continue` reported `running`/`host-paused=no`; the next query stayed `running`.
- MCP `debug_status`, `debug_stop` and `debug_continue` returned the same values. The tool list marks `debug_status`
  read-only and both control tools destructive.
- `capabilities` listed `DebugStateRequest` (`0x003B`) and `DebugControlRequest` (`0x003C`).
- A request for a missing target failed with `target 'does-not-exist' is not available`.

### Register validation

On 2026-11-09 against the same `windows` target (12 vCPUs), with the rebuilt host installed:

- `debug-registers` while the guest was running failed with `the guest must be paused before its registers can be read`.
- `debug-stop` reported `paused`, `host-paused=yes`.
- `debug-registers` returned 12 records in one response. vCPU 0 matched QEMU's `info registers` exactly for the fields
  compared: `RIP=0xfffff800aa6ac7cf`, `RSP=0xfffff8003c8ca368`, `RBP=0xfffff8003c8ca4a0`, `RFL=0x00040282`,
  `CR0=0x0000000080050033`, `CR3=0x00000000001ae000`, `CR4=0x0000000000350ef8`, and the general registers shown by
  both.
- `debug-registers --vcpu 1` returned one record (`thread p01.02`, `RIP=0xfffff800aa6acae1`, `CR3=0x00000000001ae000`).
- After the read, `debug-status` still reported `paused`, `host-paused=yes`, `stub-active=no`. HMP `info chardev`
  had no `gdb:` line, and `/tmp/mpfshost-gdb-<pid>.sock` was gone.
- `debug-continue` reported `running`, `host-paused=no`, and the next status stayed `running`.

## Stub endpoint and ownership

`debug-registers` starts a stub only after `query-status` reports `paused` and `info chardev` has no `gdb:` entry.
It asks HMP for `gdbserver unix:/tmp/mpfshost-gdb-<qemu-pid>.sock,server=on,wait=off`. If QEMU cannot create that
chardev, the host retries with `gdbserver tcp:127.0.0.1:<ephemeral-port>`. It never uses the default `tcp::<port>`
form, which listens on every interface.

The host records whether it started the stub. It stops a stub with `gdbserver none` on every exit path (success,
read error, timeout, cancellation and teardown) and removes the Unix socket file. It never stops a stub it did not
start; if `info chardev` already shows a `gdb:` device, `debug-registers` answers `InvalidState` instead. After
teardown it re-checks `query-status` and reapplies `stop` if the guest somehow resumed, so a request that began with
a paused guest leaves it paused.

## Host shutdown and stale stubs

A graceful host shutdown runs `DebugController::shutdown` from the `VmmHandle` destructor, after every session has
ended. It stops a stub this host owns, removes `/tmp/mpfshost-gdb-<pid>.sock`, and resumes the guest when the host
paused it. It does not touch a stub owned by another process.

A killed host or a crash cannot run that teardown. The guest may stay paused, and QEMU may still expose the old
`gdb:` chardev. The next host sees the stub as `stub-active=yes  stub-owned=no  foreign-stub=yes` and refuses
`debug-registers`, `debug-watch`, `debug-break` and `debug-step` with `InvalidState`. Recover explicitly:

```sh
./build/mpfshost --client debug-status <vm>
./build/mpfshost --client debug-detach <vm>
./build/mpfshost --client debug-status <vm>
```

`debug-detach` stops the foreign stub and reports `stub-stopped=yes  foreign-stub=yes`. It resumes the guest only if
this host process was the one that paused it, which is never true after a restart; run `debug-continue` when the
status still reports a stopped guest.

## Stub probe record

Checked on 2026-11-09 against a non-critical `mint` guest (QEMU 10.2.2, 8 vCPUs):

- HMP accepted `gdbserver unix:/tmp/mpfshost-gdb-probe.sock,server=on,wait=off` and answered `Waiting for gdb
  connection on device ...`.
- An active stub is visible in HMP `info chardev` as a line starting `gdb: filename=...`. QMP `query-status` stayed
  `paused` while the stub was active and attached, and HMP `info gdbstub` does not exist. A second `gdbserver` while
  one was active failed with `duplicate yank instance` / `gdbstub: couldn't create chardev`.
- `target.xml` is a wrapper with `<xi:include href="i386-64bit.xml"/>`. The include exposes the general registers and
  `cr0`, `cr2`, `cr3`, `cr4`, `cr8` and `efer`. `cr3` has `regnum="29"`.
- The `g` packet is 608 bytes of hex. The relevant offsets are `rip` 128, `cr0` 188, `cr3` 204, `cr4` 212 and
  `cr8` 220. The 8 vCPUs appeared as threads `p01.01` to `p01.08`.

## Watch stop attribution

QEMU names the watched address in a write-watchpoint stop, for example
`T05thread:p01.01;watch:1d51bce0000;`. The host accepts a hit when that address equals the requested address and
the watched value changed. It falls back to the older CR3-or-module rule only when the stub omits the field. The
field is stronger evidence than the process DTB, which can be a shadow page table that MemProcFS does not report.

## Manual check: write watchpoint

This procedure needs a live Windows guest and root. It was run against `windows` on 2026-11-09; the result is in
the validation record below. The guest probe now makes the trigger deterministic, so a live re-run no longer needs
Notepad or injected keystrokes.

1. Start the guest probe (all features are on by default; see `tools/guest-probe/README.md`). It prints
   `PROBE pid=<pid> address=0x<buffer>` and `PROBE-TRIGGER address=0x<trigger>`. Its heartbeat toggles the trigger
   byte between `0x5a` and `0xa5` every 500 ms.
2. Start the watch on the trigger address with a dedicated connection:

   ```sh
   ./build/mpfshost --client debug-watch <vm> --pid <pid> --address <PROBE-TRIGGER address> --width 1 --max-hits 1 --timeout 30
   ```

3. Wait for the next heartbeat. Expect one `DebugWatchData` frame with `rip`, `rsp`, `cr3`, old/new values, module
   and nearest export, followed by `completed  hits=1  skipped=0`. The trap should resolve inside `guest-probe.exe`
   near `GuestProbeHeartbeat`; the heartbeat uses a dedicated page, so the probe's own `CHANGE` watch does not fire
   for it.
4. Confirm the trap PC is the instruction after the store and that `before` holds the preceding bytes.
5. Afterwards check that the VM is running again, the stub is gone and the watch count is zero:

   ```sh
   ./build/mpfshost --client debug-status <vm>
   ```

6. Run the same sequence through MCP `debug_watch`. Confirm that cancelling the client (Ctrl-C) still leaves the VM
   running and the stub stopped.

Only one watch runs at a time: a second debug operation is refused with `Busy`, so a watch holds one of the four
hardware debug-register slots. QEMU's debug registers are global rather than per vCPU; a second `Z2` for the same
address on another thread is rejected with `E22`, so the host arms the watchpoint once. If the guest already uses
that slot, arming fails and the request reports an error instead of silently missing writes.

Record here: the observed `cr3` values, whether the first hit was a reallocation vs the logical store, and whether
repeated hits kept the guest responsive.

### Watchpoint validation record

On 2026-11-09 against `windows` (12 vCPU, QEMU 10.2.2) the live checks found and fixed four host-side bugs:
arming `Z2` once per guest (a duplicate on another thread returns `E22`), using `vCont;c` instead of `c`, closing the
RSP connection before stopping the stub, and pausing the guest via QMP before removing the watchpoint. A raw RSP
session with `Z2` + `vCont;c` produced a real `T05thread:p01.07;watch:<address>;` stop for a Notepad buffer write,
proving the QEMU side and the register layout.

The earlier host-end-to-end runs missed the write because QMP accepts only one client. The host keeps its QMP socket
open for the whole watch, so the external connection used to send `sendkey` only completed after the watch had
already timed out. The apparent short post-resume window was a trigger artifact, not debug-register context
switching.

The final live procedure injects keys over the SPICE inputs channel, which does not use QMP. With a Notepad buffer at
`0x20e6f5ac6a2` and a two-byte watch, a key typed well after arming produced:

```text
hit 0  vcpu 2  thread p01.03
  address=0x0000020e6f5ac6a2  old=0x0000000000000071  new=0x0000000000000072
  rip=0x00007ffe03ddb1f1  rsp=0x000000fd6f5cf418  cr3=0x0000000354cd9000
  module=msvcrt.dll+0x7b1f1
  export=memcpy+0x71
  before=03c8448a5c11ff48ffc949ffc8448819
completed  hits=1  skipped=0
```

The hit was the buffer update performed by Notepad, not the later logical character store. Afterwards
`debug-status` reported `running`, `stub-active=no` and `watches=0`; HMP `info registers` showed `DR7=0x400` and all
four address registers zero. A separate run that typed at the start of the buffer reported `old=0x78 new=0x79`, so
the old/new pair does reach `DebugWatchData`.

Two follow-up hardenings came out of the same session. A stop QEMU queued before the client attached is drained
before the first `vCont;c`, so it cannot be reported as a hit with stale registers. If a hit's CR3 does not match the
process DTB, the host now also requires a value change **and** a RIP inside one of the target's modules; a value
change alone accepted one stale idle-loop stop (`CR3=0x1ae000`, kernel `rip`). The stricter path was installed and
re-run successfully after this record was written.

## Manual check: breakpoint and single-step

This procedure needs a live Windows guest and root. Breakpoints are one-shot: each request arms one, reports the
first hit, and removes it again.

1. Start the guest probe and use its `PROBE pid=<pid>` and
   `PROBE-EXPORT name=GuestProbeMarker address=<hex>`. The heartbeat calls the exported marker once per interval.
   This target is inside the probe module, which the current breakpoint matcher requires; see the validation record
   below for the open user-mode hardware breakpoint issue.
2. Run the breakpoint with a dedicated connection:

   ```sh
   ./build/mpfshost --client debug-break <vm> --pid <pid> --address <GuestProbeMarker address> --kind hardware --timeout 30
   ```

   Expect one hit with `rip` equal to the address, the per-vCPU registers, `module`/`export`/`symbol` when they
   resolve, and `skipped=0`. Repeat with `--kind software`; the marker runs in the target process, so the software
   form should be reachable, but the `Z0` address-space limitation below still applies. Confirm the guest is running
   again and the stub is gone:

   ```sh
   ./build/mpfshost --client debug-status <vm>
   ```

3. For a single step, pause the guest, then step once per call:

   ```sh
   ./build/mpfshost --client debug-stop <vm>
   ./build/mpfshost --client debug-registers <vm> --vcpu 0
   ./build/mpfshost --client debug-step <vm> --vcpu 0
   ```

   Expect the full register record with a new `rip`; the guest stays paused. A `debug-step` while running must fail
   with `InvalidState`. Finish with `debug-continue`.

4. Run the same sequence through MCP `debug_break` and `debug_step`. Confirm both are marked destructive, and that
   cancelling `debug_break` with Ctrl-C still leaves the VM running and the stub stopped.

### Breakpoint limits and interactions

- A hardware breakpoint (`Z1`) consumes one of the four debug-register slots. The host refuses concurrent debug
  operations with `Busy`, so a watchpoint and a breakpoint cannot compete for a slot in the same request, but a
  guest kernel debugger or a guest driver can still own the register the host tries to use; arming then fails
  instead of silently missing the breakpoint.
- A software breakpoint (`Z0`) patches the instruction byte. QEMU applies the patch through the address space of the
  vCPU the stub currently has selected, so a user-mode address can be rejected with `E22` when that vCPU is not in the
  target process. Kernel addresses work because the kernel CR3 maps them, and the host logs the stub reply. Use the
  hardware form for user-mode addresses.
- A hardware breakpoint is armed once and fires globally, like a watchpoint. The host requires the stop PC to equal
  the requested address, so an unrelated vCPU stop is not reported as a hit. It then accepts a hit when CR3 matches the
  process or when the address is inside one of the target's modules, which covers a shadow user CR3. The same
  reallocation/relocation caveat as a watchpoint applies.
- Snapshots, `system_reset` and migration drop debug registers and the stub. Treat every breakpoint, watchpoint and
  step as ephemeral and tear it down before one of those operations.
- Single-step requires a paused guest. It selects one thread, executes one instruction, captures registers and leaves
  the guest stopped. Other vCPUs are not stepped.

### Breakpoint and step validation record

Run on 2026-11-09 against the `windows` target (12 vCPU, QEMU 10.2.2):

- Hardware `debug-break` on `ntoskrnl.exe!HalProcessorIdle+0xf` (`0xfffff800aa6ac7cf`, pid 4) hit on vCPU 1/thread
  `p01.02` with `rip` equal to the requested address, `cr3=0x1ae000`, `skipped=0`, and `bytes=c3cccccccccccc...`.
  The guest was running afterwards, the stub was gone, and a second run while the guest was paused left it stopped.
- Software `debug-break` on the same kernel address hit on vCPU 3/thread `p01.04`, and the first byte was restored to
  `c3` after teardown. A software breakpoint on a user address in `ntdll.dll` failed with `E22` because the selected
  vCPU was not in that process, which confirms the Z0 address-space limitation above.
- The first live run exposed a false positive: a stop with the target CR3 but a different PC was reported as a hit.
  The host now requires the stop PC to equal the breakpoint address; a regression test covers it. After that change,
  repeated hardware runs reported only the requested address.
- `debug-step --vcpu 0` and `--vcpu 10` each advanced `rip` from the idle loop (`0xfffff800aa6ac7cf`) to
  `0xfffff800aa4af69b`, returned the full register record, left the guest stopped, and `debug-continue` resumed it.
- A hardware breakpoint on a cold kernel address timed out with `skipped=0` and left the guest running. Killing the
  client during a pending breakpoint (`timeout 2 ...`) also left the guest running with `stub-active=no`.
- MCP `debug_break` returned the same hit structure as the CLI. MCP `debug_step` returned the same register record as
  `debug-registers` for the stepped vCPU.

### Guest-probe fixture validation record

Run on 2026-11-09 against the `windows` target (12 vCPU) with `guest-probe.exe` pid 2036 and the first fixture
build:

- The startup inventory matched every value used later: module base, exports, regions, pattern, trigger and thread
  ids.
- `processes`, `proc-info`, `modules --counts`, `sections`, `eat`, `iat`, `symbol`, `resolve` and `module-at`
  resolved the probe and its three exports.
- `read`, `write`, `scatter`, text `search` and wildcard hex `search` all hit the expected bytes in the main buffer.
- `vquery`, `maps`, `vadex`, `pte` and `heap` reported the expected regions. A `page` read of `noaccess`, `guard` and
  `reserve` still succeeded, because MemProcFS reads guest physical memory rather than the guest's protection view.
- `threads` showed the main, heartbeat, watch and two worker threads; `threads --callstack` resolved the wait paths.
- `debug-status`, `debug-stop`, `debug-registers`, `debug-step` and `debug-continue` behaved normally.
- `debug-watch` on the trigger byte (width 1, 10 s) and on `GuestProbeCounter` (width 8, 15 s) armed and trapped at
  the 500 ms heartbeat rate, but every stop was rejected: `hits=0 skipped=20` and `hits=0 skipped=29`.
- `debug-break --kind hardware` on `GuestProbeMarker` timed out with `skipped=4705` in the first run, and the
  heartbeat counter barely advanced during the run.
- A second run used the updated fixture (pid 248) with the heartbeat pinned to vCPU 0. The RX-page
  `PROBE-BREAKPOINT` stub timed out with `skipped=3295`, and a hardware breakpoint on
  `ntdll!RtlEnterCriticalSection` timed out with `skipped=1612`.
- `module-at` rejects the RX stub because it is not in a loaded module, so the breakpoint matcher cannot accept it
  even though the stop PC matches. A later run on the module marker itself, with a fresh module base, hit
  immediately on vCPU 0 with `skipped=0` and resolved `GuestProbeMarker+0x0`. The first marker timeout was therefore
  a shared-address stop storm, not a failure of the module breakpoint path.
- `debug-break --kind software` on `GuestProbeMarker` was rejected with `E22`, as the `Z0` address-space limitation
  predicts.
- A control hardware breakpoint on `ntoskrnl.exe!HalProcessorIdle+0xf` hit immediately with `skipped=0`, so the
  breakpoint path itself works for a kernel address that matches the process DTB.
- A raw RSP capture of the rejected watchpoint stop showed
  `T05thread:p01.01;watch:1d51bce0000;`, `rip` inside `GuestProbeHeartbeat`, `cr3=0x207588000` equal to the process
  DTB, and the new trigger value readable through MemProcFS. The host now accepts on that watch address instead of
  relying on the CR3/module guess.
- After installing the rebuilt host, `debug-watch` on the current probe (pid 2780, trigger `0x2635ae10000`, width 1,
  `--max-hits 2`) completed with two hits and `skipped=0`. Each hit reported `rip=0x7ff7850e14a5`,
  `cr3=0x366454000`, `module=guest-probe.exe+0x14a5`, `export=GuestProbeHeartbeat+0x1d`, and the decoded store
  `mov byte [rdx], al`; old/new toggled `0x5a`/`0xa5`.
- `debug-break --kind hardware` on `GuestProbeMarker` (`0x7ff7850e1480`) hit on vCPU 0/thread `p01.01` with
  `rip` equal to the requested address, `cr3=0x366454000`, `module=guest-probe.exe+0x1480`,
  `export=GuestProbeMarker+0x0`, and `skipped=0`.

## Checks still owed to a live guest

- ~~Whether `Z2` watchpoints must be armed per vCPU.~~ Answered on 2026-11-09 against `windows` (12 vCPU): a first
  `Z2,<addr>,2` on one thread returns `OK`; repeating it for the same address on another thread returns `E22`.
  QEMU therefore keeps one global debug-register set, and the host arms the watchpoint once.
- Whether a `debug-stop` pause interacts badly with snapshots, `system_reset` or migration. A reset or snapshot drops
  debug registers and the stub; treat a watch as ephemeral and tear it down before either operation.
- Characterize whether a hardware breakpoint on a hot shared user address (such as `ntdll!RtlEnterCriticalSection`)
  can ever be attributed to the target process. It traps in every process that maps the address; a fresh probe
  restart gives the module marker a new ASLR base if a shared base causes a stop storm.
- The first watch hit immediately after a host restart once reported RIP and CR3 from the previous probe process;
  a rerun was correct. Check whether a queued stop needs draining when a new stub session starts.

Record the remaining answers here after the next live run.

## Teardown rules for later slices

Any future slice that starts a stub or arms a watchpoint must:

- remember whether the host started the stub and never stop one it did not start;
- remove the watchpoint or breakpoint, close the RSP connection, then stop the stub; otherwise QEMU can stay in its
  gdb stop state and ignore the resume;
- stop the stub on success, timeout, cancellation, error and client death;
- stop an owned stub and resume a host-paused guest during host shutdown; use `debug-detach` to stop a stale foreign
  stub, never the shutdown path;
- resume the VM if the request paused it, re-pause an already-paused guest, then verify with `query-status`;
- refuse concurrent debug operations with `Busy`;
- never log guest memory contents.
