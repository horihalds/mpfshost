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

## Checks still owed to a live guest

These were listed as prerequisites for the later debug slices and have not yet been verified on this machine:

- Whether HMP accepts `gdbserver unix:<path>` as a chardev target, and how to detect an already-active stub.
- Whether `Z2` watchpoints must be armed per vCPU.
- Whether `target.xml` exposes `cr3`.
- Whether a `debug-stop` pause interacts badly with snapshots, `system_reset` or migration.

Record the answers here before starting the GDB stub slice. Until then, treat the stub-related host state as
reserved.

## Teardown rules for later slices

Any future slice that starts a stub or arms a watchpoint must:

- remember whether the host started the stub and never stop one it did not start;
- remove watchpoints and stop the stub on success, timeout, cancellation, error and client death;
- resume the VM if the request paused it, then verify with `query-status`;
- refuse concurrent debug operations with `Busy`;
- never log guest memory contents.
