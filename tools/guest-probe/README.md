# guest-probe

`guest-probe.exe` is a Windows fixture for live mpfshost tests. It gives the host known memory regions, exports,
threads and a deterministic trigger so memory, map, symbol and debug commands can be exercised without guessing
addresses or typing into the guest.

## Build and copy

The normal build produces `build/guest/guest-probe.exe` when `x86_64-w64-mingw32-g++` is available:

```sh
ninja -C build guest-probe
```

Copy the `.exe` into the guest and run it from a console. Every feature is enabled by default; the simplest full
fixture is:

```text
guest-probe.exe
```

That creates the buffer and regions, starts the heartbeat on vCPU 0, creates two worker threads and mirrors output
to `%TEMP%\guest-probe-<pid>.log`.

## Options

| Option | Default | Meaning |
|--------|---------|---------|
| `--size <bytes>` | `4096` | Size of the main buffer (max 16 MiB). |
| `--magic <hex byte>` | `0xa5` | Byte the main buffer is filled with. |
| `--pattern` / `--no-pattern` | on | Overlay a known 64-byte pattern at offset 0 of the main buffer. |
| `--regions` / `--no-regions` | on | Create the `ro`, `rx`, `noaccess`, `guard` and `reserve` regions. |
| `--watch` / `--no-watch` | on | Scan the main buffer for external writes and print `CHANGE` lines. |
| `--watch-interval <ms>` | `500` | Delay between main-buffer scans (minimum 50 ms). |
| `--heartbeat <ms>` / `--no-heartbeat` | `500` | Toggle the trigger byte and call `GuestProbeMarker`; `0` disables it. |
| `--heartbeat-cpu <n>` | `0` | Pin the heartbeat thread to this vCPU (maximum 63). |
| `--threads <n>` | `2` | Idle worker threads (maximum 64). |
| `--duration <seconds>` | infinite | Exit cleanly after this many seconds. |
| `--log <path>` / `--no-log` | temp file | Mirror output to a file. |
| `--help` | | Show the option list. |

The process stops on `Ctrl-C` or after `--duration` and prints `PROBE-DONE reason=signal|duration`.

## Output

All inventory lines are `key=value` with numeric addresses in `0x` hex. They are emitted once at startup.

```text
PROBE-LOG path=C:\Users\...\guest-probe-1234.log
PROBE version=2 pid=1234 address=0x123000 size=4096 magic=0xa5
PROBE-MODULE name=guest-probe.exe base=0x140000000 size=0x4f000 path=C:\...\guest-probe.exe
PROBE-EXPORT name=GuestProbeMarker address=0x140001480
PROBE-EXPORT name=GuestProbeHeartbeat address=0x140001488
PROBE-EXPORT name=GuestProbeCounter address=0x140010040
PROBE-PATTERN offset=0x0 size=64 hex=4d504653...
PROBE-REGION name=main address=0x... size=4096 state=commit protection=rw
PROBE-REGION name=ro address=0x... size=4096 state=commit protection=r
PROBE-REGION name=rx address=0x... size=4096 state=commit protection=rx
PROBE-BREAKPOINT address=0x...
PROBE-REGION name=noaccess address=0x... size=4096 state=commit protection=noaccess
PROBE-REGION name=guard address=0x... size=4096 state=commit protection=guard
PROBE-REGION name=reserve address=0x... size=4096 state=reserve protection=noaccess
PROBE-REGION name=trigger address=0x... size=4096 state=commit protection=rw
PROBE-TRIGGER address=0x... width=1 interval-ms=500 cpu=0
PROBE-THREAD role=heartbeat index=0 tid=...
PROBE-THREAD role=watch index=0 tid=...
PROBE-THREAD role=worker index=0 tid=...
PROBE-THREAD role=worker index=1 tid=...
```

`PROBE-BREAKPOINT` is the entry of a small `endbr64; ret` stub in the probe's own RX page. It is called by the
heartbeat every interval, so it is a breakpoint target whose address is not shared with another process's module.
The exported `GuestProbeMarker` remains available for symbol and export tests.

`CHANGE offset=0x.. old=0x.. new=0x..` is printed for every byte of the main buffer changed from outside. The
heartbeat writes to the separate `trigger` region, so `--watch` does not report the probe's own heartbeat.

## Recipes

The examples use target `win11` and the values printed by the probe. Replace `1234` and the addresses with the
actual `PROBE` and `PROBE-...` lines.

### Process, memory, scatter and search

```sh
./build/mpfshost --client processes win11 --name guest-probe
./build/mpfshost --client read win11 --pid 1234 --address 0x123000 --size 64
./build/mpfshost --client write win11 --pid 1234 --address 0x123000 --data "de ad be ef"
./build/mpfshost --client scatter win11 --pid 1234 --range 0x123000:64 --range 0x124000:16
./build/mpfshost --client search win11 --pid 1234 --pattern "MPFSHOST-GUEST-PROBE" --max-results 8
./build/mpfshost --client search win11 --pid 1234 --hex-pattern "4d504653484f5354????????????????" --max-results 8
```

The pattern prefix is `MPFSHOST-GUEST-PROBE` (`4d504653484f53542d47554553542d50524f4245`), so both text and
wildcard hex searches have a known hit.

### Regions, maps and page tables

```sh
./build/mpfshost --client vquery win11 --pid 1234 --address <ro region>
./build/mpfshost --client vquery win11 --pid 1234 --address <guard region>
./build/mpfshost --client maps win11 --pid 1234
./build/mpfshost --client vadex win11 --pid 1234
./build/mpfshost --client pte win11 --pid 1234
./build/mpfshost --client page win11 --pid 1234 --range <noaccess region>:4096
./build/mpfshost --client virt2phys win11 --pid 1234 --address <main address>
```

The probe never touches the `noaccess`, `guard` and `reserve` regions after protecting them, so `vquery` and the
map/PTE views report their protections accurately. A `page` read may still succeed, because MemProcFS reads guest
physical memory instead of going through the guest's protection checks; use these regions for protection and
metadata tests, not as read-denial tests. The `rx` region is executable but is not called.

### Module, symbols and exports

```sh
./build/mpfshost --client modules win11 --pid 1234 --counts
./build/mpfshost --client sections win11 --pid 1234 --module guest-probe.exe
./build/mpfshost --client eat win11 --pid 1234 --module guest-probe.exe
./build/mpfshost --client iat win11 --pid 1234 --module guest-probe.exe
./build/mpfshost --client symbol win11 --pid 1234 --module guest-probe.exe --symbol GuestProbeMarker
./build/mpfshost --client resolve win11 --pid 1234 --address <GuestProbeMarker address>
./build/mpfshost --client module-at win11 --pid 1234 --address <GuestProbeMarker address>
```

The exported `GuestProbeMarker`, `GuestProbeHeartbeat` and `GuestProbeCounter` names are stable. The probe prints
their runtime addresses, so ASLR does not matter.

### Threads

```sh
./build/mpfshost --client threads win11 --pid 1234
./build/mpfshost --client threads win11 --pid 1234 --callstack
```

Use `--threads 8` when you need more thread records.

### Write watchpoint

Arm the watch on the trigger byte. The heartbeat writes it every 500 ms, so the watch hits without any guest input:

```sh
./build/mpfshost --client debug-watch win11 --pid 1234 --address <PROBE-TRIGGER address> \
    --width 1 --access write --max-hits 2 --timeout 30
```

The trap should resolve inside `guest-probe.exe` (near `GuestProbeHeartbeat`). The old/new pair toggles between
`0x5a` and `0xa5`.

### Breakpoint and single step

Use `PROBE-BREAKPOINT` as the hardware breakpoint target. It is called once per heartbeat and lives in a page owned
only by the probe, so a global hardware breakpoint cannot collide with a hot address in another process:

```sh
./build/mpfshost --client debug-break win11 --pid 1234 --address <PROBE-BREAKPOINT address> \
    --kind hardware --timeout 30
```

`GuestProbeMarker` is still exported and callable, but its address is inside `guest-probe.exe`; on a guest where
another process maps a module at the same base, a hardware breakpoint there can trap far more often than the
heartbeat. The guest must be running before the breakpoint is armed. For single-step checks, use the same
paused-guest sequence
as any other process:

```sh
./build/mpfshost --client debug-stop win11
./build/mpfshost --client debug-registers win11 --vcpu 0
./build/mpfshost --client debug-step win11 --vcpu 0
./build/mpfshost --client debug-continue win11
```

### Log

With the default `--log`, the probe writes the same lines to `%TEMP%\guest-probe-<pid>.log` and prints the exact
path in `PROBE-LOG`. If your MemProcFS VFS view exposes a filesystem module, that file can be read with `vfs-read`
without a network or serial channel. The stock MemProcFS VFS has no filesystem view, so this is only available when
that module is present.

## Notes

- The probe is a Windows program; it is cross-compiled with mingw and is never linked into the host or the tests.
- `PROBE-BREAKPOINT` only exists with `--regions` (the default). `--no-regions` removes it along with the protection
  regions.
- The heartbeat is pinned to vCPU 0 by default. If a debug test targets a different vCPU, pass `--heartbeat-cpu <n>`
  and use the `cpu=` value in `PROBE-TRIGGER`.
- A heartbeat interval below 500 ms makes a watch hit sooner, but also makes `--max-hits` fill quickly.
- The live results and the remaining debug-controller issue are recorded in
  [`docs/DEBUGGING.md`](../../docs/DEBUGGING.md).
- The `guard` and `noaccess` pages immediately fault on access from inside the guest; do not add code that touches
  them.
- The probe stays idle except for the heartbeat and worker threads. Use `--duration` for scripted runs that should
  exit by themselves.
