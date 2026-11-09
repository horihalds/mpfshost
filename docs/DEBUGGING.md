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

## Manual check: state, stop and continue

1. Start the host (root) and confirm the target is visible:

   ```sh
   ./build/mpfshost --client list-targets
   ```

2. With the guest running:

   ```sh
   ./build/mpfshost --client debug-status <vm>
   ```

   Expect `run-state=running`, `host-paused=no`, `stub-active=no`, `vcpus=<n>` matching the QEMU command line, and
   `watches=0`.

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

- `debug-status` reported `running`, `host-paused=no`, `stub-active=no`, `watches=0`.
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

## Manual check: write watchpoint

This procedure needs a live Windows guest and root. It was run against `windows` on 2026-11-09; the result is in
the validation record below.

1. Pick a process and a writable buffer, and confirm the address with `read_memory`. A UTF-16 text buffer with spare
   capacity is easier than one that is about to be reallocated.
2. Start the watch with a dedicated connection:

   ```sh
   ./build/mpfshost --client debug-watch <vm> --pid <pid> --address <hex> --width 2 --max-hits 1 --timeout 30
   ```

3. Trigger the write from another shell or from the guest desktop (a keystroke works for an edit control). Expect one
   `DebugWatchData` frame with `rip`, `rsp`, `cr3`, old/new values, module and nearest export, followed by
   `completed  hits=1  skipped=0`.
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

1. Pick a frequently executed address in a process, such as an export confirmed with `resolve`. Note the pid and the
   address.
2. Run the breakpoint with a dedicated connection:

   ```sh
   ./build/mpfshost --client debug-break <vm> --pid <pid> --address <hex> --kind hardware --timeout 30
   ```

   Expect one hit with `rip` equal to the address, the per-vCPU registers, `module`/`export`/`symbol` when they
   resolve, and `skipped=0`. Repeat with `--kind software`; a read-only page may reject the software form. Confirm
   the guest is running again and the stub is gone:

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
- A software breakpoint (`Z0`) patches the instruction byte. It needs a writable page and is removed on every exit
  path; a page that rejects the patch makes the request fail.
- A breakpoint is scoped by CR3 after the hit. A hit in another process that maps the same address is skipped, unless
  the hit is at the requested address inside one of the target's modules, which covers a shadow user CR3. The same
  caveat as a watchpoint applies: reallocation or relocation of executable memory invalidates the address.
- Snapshots, `system_reset` and migration drop debug registers and the stub. Treat every breakpoint, watchpoint and
  step as ephemeral and tear it down before one of those operations.
- Single-step requires a paused guest. It selects one thread, executes one instruction, captures registers and leaves
  the guest paused. Other vCPUs are not stepped.

### Breakpoint and step validation record

Record the live results here after the next run: the vCPU and thread of the hit, whether `rip` matched the requested
address for both kinds, whether a software breakpoint survived the target page's permissions, and whether the guest
was running and the stub gone afterwards.

## Checks still owed to a live guest

- ~~Whether `Z2` watchpoints must be armed per vCPU.~~ Answered on 2026-11-09 against `windows` (12 vCPU): a first
  `Z2,<addr>,2` on one thread returns `OK`; repeating it for the same address on another thread returns `E22`.
  QEMU therefore keeps one global debug-register set, and the host arms the watchpoint once.
- Whether a `debug-stop` pause interacts badly with snapshots, `system_reset` or migration. A reset or snapshot drops
  debug registers and the stub; treat a watch as ephemeral and tear it down before either operation.

Record the remaining answers here after the next live run.

## Teardown rules for later slices

Any future slice that starts a stub or arms a watchpoint must:

- remember whether the host started the stub and never stop one it did not start;
- remove the watchpoint or breakpoint, close the RSP connection, then stop the stub; otherwise QEMU can stay in its
  gdb stop state and ignore the resume;
- stop the stub on success, timeout, cancellation, error and client death;
- resume the VM if the request paused it, re-pause an already-paused guest, then verify with `query-status`;
- refuse concurrent debug operations with `Busy`;
- never log guest memory contents.
