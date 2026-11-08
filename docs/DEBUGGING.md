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
| Registers, watchpoints, stepping | GDB remote serial protocol | see later slices |

`gdbserver` is **HMP-only** in the QEMU versions tested; there is no QMP-native equivalent. The host therefore has to
start and stop a stub through `human-monitor-command`. Task 1 (state, stop, continue) does not start a stub, so
`stubActive` stays false and `watchCount` stays zero.

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

## Checks still owed to a live guest

- Whether `Z2` watchpoints must be armed per vCPU. Verify in the watchpoint slice.
- Whether a `debug-stop` pause interacts badly with snapshots, `system_reset` or migration.

Record the answers here when the watchpoint slice reaches them.

## Teardown rules for later slices

Any future slice that starts a stub or arms a watchpoint must:

- remember whether the host started the stub and never stop one it did not start;
- remove watchpoints and stop the stub on success, timeout, cancellation, error and client death;
- resume the VM if the request paused it, then verify with `query-status`;
- refuse concurrent debug operations with `Busy`;
- never log guest memory contents.
