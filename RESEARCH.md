# Research: guest debug support for mpfshost

Date: 2026-11-08
Status: exploratory; not implemented. Nothing here is a commitment to a protocol change.

## Question

Could mpfshost gain debug capabilities — stop/continue, registers, breakpoints, watchpoints,
single-step — and how should they be split between QEMU's debug channels and MemProcFS
introspection?

The concrete experiment was: **find out what writes to a specific address in a specific Windows
process**, using a live QEMU/KVM guest, QEMU's GDB stub, and the existing mpfshost MCP tools.

## Summary

- The combination works. mpfshost found the target buffer; QEMU's `gdbserver` armed a hardware
  write watchpoint; the write was caught; mpfshost resolved the hit instruction to a module and
  nearest export.
- The split is natural: **QEMU/GDB owns CPU state and traps; mpfshost owns process context,
  memory reads, modules, exports and symbols.**
- The hard parts are not the watchpoint itself. They are: arming only while the VM is stopped,
  scoping a linear-address watchpoint to one process, capturing registers at the exact hit under
  SMP, and symbolizing without a symbol server round trip.
- Adding debug support to mpfshost is feasible as an orchestration layer over QMP/HMP plus
  existing MemProcFS providers. It should not try to be a CPU debugger.

## Environment used

- QEMU 10.2.2, `oob` capability, QMP over a Unix socket (group `qemu`).
- 12 vCPUs, 32 GiB guest RAM, Windows guest.
- AMD GPU passed through; guest desktop is not on QEMU's emulated display.
- QMP `query-display-options` reports `type: none`; VNC disabled; SPICE present on 5900.
- `gdb` 18.1 on the host.
- mpfshost MCP server with the 50-tool catalogue (writes exposed).

## 1. QEMU control plane

### 1.1 What QMP/HMP exposes

`query-commands` advertised 243 commands in this build. The ones relevant to debugging:

| Capability | Command |
|---|---|
| Stop / resume | `stop`, `cont`, `system_reset`, `system_powerdown`, `inject-nmi` |
| CPU debugger | HMP `gdbserver [device]` (no QMP-native equivalent), HMP `gdbserver none` |
| Registers / physical memory | HMP `info registers`, HMP `xp` via `human-monitor-command` |
| Memory dumps | `dump-guest-memory`, `memsave`, `pmemsave` |
| Input injection | `send-key`, `input-send-event` (absolute pointer events included) |
| Screen capture | `screendump` |
| Save/restore | `snapshot-save`, `snapshot-load`, `snapshot-delete`, `migrate*` |
| Device control | `device_add`, `device_del`, `qom-get`, `qom-set`, `netdev_add` |
| Introspection | `query-status`, `query-cpus-fast`, `query-block`, `query-pci`, `query-memory-size-summary` |

Important: `gdbserver` is an **HMP** command. Reaching it from QMP means
`human-monitor-command` (or a chardev configured at startup). A debug feature cannot assume a
QMP-native stub control.

### 1.2 gdbserver basics confirmed

```text
gdbserver tcp:127.0.0.1:1234     # start, localhost only
gdbserver tcp::1234              # default: all interfaces — avoid
gdbserver none                   # stop
```

- Attaching exposes one GDB thread per vCPU, e.g. `Thread 1.9 (CPU#8 [halted])`.
- `$rip`, `$cr3`, `$rsp` and other registers are readable.
- Memory reads use the **current vCPU's CR3**. Kernel addresses work from any context; another
  process's user virtual address often does not.
- On x86_64, a watchpoint hit reports the **instruction after** the store (debug traps are
  trap-after). Read backwards a few bytes to find the actual writing instruction.
- Watchpoints are hardware debug-register watchpoints: at most 4, widths 1/2/4/8 bytes, and
  they compare **linear addresses**. They are global to the VM, not scoped to a process.

### 1.3 Security

The QMP socket is the hypervisor control plane. Whoever can connect can reset the VM, dump RAM,
inject input, start a GDB stub, hotplug devices, and change disks. Treat socket access as
root-equivalent over the guest. The same applies to `gdbserver`: it is unauthenticated and
defaults to listening on all interfaces. Bind it to loopback or a Unix chardev, and stop it when
done. Any mpfshost feature that exposes debug control needs the same opt-in treatment as
`--allow-vfs-write`, plus logging.

## 2. The watchpoint experiment

### 2.1 Setup

1. Located a live UTF-16 text buffer in `notepad.exe` with `search_memory` (see §3 for the
   pattern limit) and confirmed it with `read_memory`.
2. Paused the VM (`stop`), then started the stub on `tcp:127.0.0.1:1234`.
3. In GDB: `target remote`, then
   `watch *(unsigned short*)<terminator address>`, then `continue`.
4. Triggered a write with one keystroke (`send-key`).
5. On the hit, took `$rip` and resolved it with mpfshost.

### 2.2 Result (ephemeral addresses, illustrative only)

```text
Thread 9 (CPU#8) hit Hardware watchpoint 1
Old value = <unreadable>
New value = 0
0x00007ff84522b1f1 in ?? ()
```

- Hit thread: CPU#8.
- RIP `0x7ff84522b1f1`.
- The watched word held the NUL terminator of the text buffer.

### 2.3 Symbolization with mpfshost

- `list_modules` (pid of `notepad.exe`) → RIP falls in `msvcrt.dll`,
  base `0x7ff8451b0000`, offset `0x7b1f1`.
- `list_exports` nearest export → `memcpy` at `0x7ff84522b180`, so `memcpy+0x71`.
- `read_memory` at `0x7ff84522b1e0` returned:

```text
49 03 c8            add    rcx, r8
44 8a 5c 11 ff      mov    r11b, [rcx+rdx-1]
48 ff c9            dec    rcx
49 ff c8            dec    r8
44 88 19            mov    [rcx], r11b     <- the store
75 f0               jne    ...
c3                  ret
```

The reported RIP is the `jne` after the store. Interpretation: the edit control was moving the
text buffer; `msvcrt` wrote the terminator as part of a backwards copy. The buffer later
reappeared at a different address, confirming a reallocation. So the watchpoint caught the
**move**, not the keystroke's own byte store — a useful reminder that the first hit is often one
layer below the code you care about.

`pdb_symbol_name` could not resolve the offset (`no pdb symbol matches the address in module
'msvcrt.dll'`). The nearest-export fallback was enough here. Loading a PDB may trigger a symbol
server download; mpfshost must not do that implicitly (see §5).

### 2.4 What each tool contributed

| Need | Tool |
|---|---|
| Find the address in a process | mpfshost `search_memory`, `read_memory`, `list_heap_allocations`, `memory_map` |
| Read the right process's user memory | mpfshost (GDB is tied to the current CR3) |
| Trap the write | QEMU `gdbserver` + GDB hardware watchpoint |
| Get RIP / CR3 / RSP at the hit | GDB |
| RIP → module → export/symbol | mpfshost `list_modules`, `list_exports`, `pdb_symbol_name` |
| Stack return addresses → symbols | GDB for the stack, mpfshost for symbolization |
| VA → PA → PFN | mpfshost `virt2phys`, `list_pfn`, `list_pte` |

## 3. Operational findings and gotchas

1. **Arm the watchpoint only while the VM is stopped.** Connecting while the guest is running
   left the target in a running state; `watch`/`continue` then failed or the watchpoint never
   really armed. `stop` first, then start the stub and attach.
2. **All-stop is not what it looks like under 12 vCPUs.** GDB reports one thread per vCPU and a
   per-thread stop. A CLI `commands` block attached to the watchpoint failed with
   `Cannot execute this command while the target is running`. The same happened with a Python
   `gdb.events.stop` handler in this environment. A plain `continue` as the last batch command
   did report the hit and its PC. Capturing other registers at the hit needs either an
   interactive GDB session or a more careful stop-handling strategy; plan for this rather than
   assuming `commands` works.
3. **Capture CR3, not just RIP.** A linear-address watchpoint can fire in any process that maps
   the same virtual address. The definitive process check is the CR3 at the hit. In the
   successful run the RIP fell inside the target's `msvcrt.dll`, which corroborated the process,
   but that is weaker than matching CR3 to the process DTB.
4. **GDB cannot symbolize.** It has no PDBs and no process model. Disassembling a user address
   also failed while the current CR3 was not the target process. Read the bytes through
   mpfshost and symbolize there.
5. **Trap-after semantics.** The reported PC is after the writing instruction. Disassemble
   `rip-16 .. rip` and look for the store.
6. **Buffer relocation defeats a naive watchpoint.** Watching the terminator of a string that
   is about to be reallocated catches the copy, not the logical write. Options: watch a buffer
   with spare capacity, set a breakpoint on `memcpy`/`memmove` and filter by destination, or
   re-resolve the address after each hit.
7. **Detach/resume is not guaranteed.** After GDB exits, the VM may resume or stay paused
   depending on how the session ended. Always send `gdbserver none` and `cont`, then verify with
   `query-status`. In one failed run the VM stayed paused and the next injected keystroke was
   lost.
8. **Screendump is useless with GPU passthrough.** `screendump` captured the stale emulated
   UEFI console (1280×800, Windows Boot Manager), not the guest desktop. A debug feature must not
   rely on it for visual confirmation on this class of VM.
9. **QMP responses interleave with events.** A raw line reader must skip `SPICE_CONNECTED`,
   `RESUME`, `STOP` and similar asynchronous events until it sees `return` or `error`.
10. **KVM debug-register budget.** Four watchpoints is a hard cap, and a guest kernel debugger or
    another tool using `dr0..dr3` will conflict.
11. **Reset clears state.** `system_reset`, snapshots and migration will drop breakpoints and
    watchpoints. Debug sessions must be treated as ephemeral.

## 4. mpfshost capabilities and gaps observed

Existing and useful:

- `search_memory` with `pattern` (literal, ASCII) or `hex_pattern` (wildcards `??`).
  **Hard limit: 32 pattern bytes.** UTF-16 therefore allows only 16 characters per search, and
  non-ASCII text must be expressed as hex.
- `read_memory` / `read_physical_memory`, both capped at 1 MiB per call.
- `list_modules` (base, `image_size`), `list_exports` (field is `va_function`, not `address`),
  `list_imports`, `pdb_symbol_name`, `pdb_symbol_address`, `pdb_type_size`,
  `pdb_type_child_offset`, `module_layout`.
- `memory_map`, `list_pte`, `list_pfn`, `virt2phys`, `translate_gpa`.
- `list_processes` / `list_all_processes` (terminated processes included).

Gaps that directly affected this experiment:

- **No module-by-address lookup.** The caller must scan `list_modules` and compare base..base+size.
- **No CR3/DTB per process.** The hit's `$cr3` cannot be mapped back to a pid through the MCP
  surface. `list_processes` returns pid, parent, name and wow64 only.
- **No symbol-by-address.** `pdb_symbol_name` needs a module name and a module-relative offset;
  there is no "resolve this absolute RIP" call and no nearest-export helper.
- **No disassembler.** Bytes were read with `read_memory` and disassembled outside.
- **No stack unwinder.** Return addresses would have to be read and symbolized one by one.
- **No debug primitives.** No breakpoints, watchpoints, stop/continue, registers or stepping.

## 5. Design options for debug support

### A. Host-side orchestrator over QMP + MemProcFS (recommended)

Add a debug provider in `src/host/` that talks to QMP/HMP for CPU control and to the existing
MemProcFS providers for process context and symbols. Expose a small, deliberate set of
operations. Advantages: no new dependency, no guest agent, reuses the access policy and the
existing provider split. Risks: QMP socket availability and permissions, host-side blocking and
timeouts, and the need for a real state machine.

### B. GDB remote-protocol client in `mpfshost_core`

Speak GDB RSP directly over TCP (or a chardev) instead of shelling out to GDB. This removes the
GDB CLI race and gives precise stop-event handling. It is more code: packets, register layouts,
target XML, multiprocess/non-stop corner cases. It also needs a way to start `gdbserver`, which
is HMP-only.

### C. MemProcFS-only polling

Periodically read a range and diff it. No VM pause, no debug registers, no QMP. Useful as a
fallback and easy to test, but it cannot report the writing instruction, is racy, and is
unusable for large ranges.

### D. Guest-side or kernel-debugger integration

WinDbg/KD over a serial pipe or named pipe, or a guest driver using debug registers. Highest
fidelity for Windows internals, highest setup cost, and it conflicts with "no guest
cooperation". Out of scope for the first iteration.

## 6. Sketch of a minimal capability set

Phases, smallest first:

1. **Read-only symbol services** (valuable with or without debug):
   - `resolve_address(pid, address)` → `{module, module_offset, symbol, export, displacement}`
   - `process_for_dtb(cr3)` → pid, or `null`
   - `find_module(address)` across the process's module list
   - offline PDB cache with an explicit opt-in for network fetches
2. **CPU control, read-only-ish**: `debug_stop`, `debug_continue`, `debug_registers` (per vCPU),
   gated behind a host flag.
3. **Watchpoints**: `debug_watch(pid, address, width, access)`. Implementation must:
   - pause the VM, start/ensure the stub, arm on all vCPUs, resume;
   - wait with a deadline and cancellation;
   - on hit capture `rip`, `cr3`, `rsp`, the old/new value and the vCPU id;
   - verify the hit's CR3 against the requested pid;
   - resolve `rip` through the symbol service;
   - optionally walk the stack and symbolize return addresses;
   - always tear the stub down.
4. **Breakpoints and stepping** only after the watchpoint path is solid.

Protocol notes if this becomes a wire feature:

- Append new `MessageType` values; never renumber. `kProtocolVersion` stays 1.
- A watch is a **streamed** request: `*Event` frames followed by exactly one `*End`, or one
  `ErrorResponse` instead. Echo the request id. Check `requestAborted` before every event.
- One request at a time per connection is a poor fit for an interactive debugger. Either use a
  dedicated debug connection per watch, or define sessions with explicit ids. Cancellation must
  work while the watch is armed and while the VM is paused.
- Debug control is destructive. Follow the `--allow-vfs-write` precedent: a host flag, a warning
  at startup, `AccessDenied` otherwise, and logging of every arm/disarm/resume.

Threading and ownership:

- Do not put mutable per-session debug state on `VmmHandle`; it is shared across sessions.
- Keep declarations in `runServer` in the existing order so sessions end before handles close.
- A paused VM while a session dies is the worst failure mode. Tear-down must resume the VM even
  on error paths.

## 7. Testing strategy

- Everything in `mpfshost_core` can be tested without a VM: the RSP/QMP framing, the debug state
  machine, address→module matching, symbol-cache policy, cancellation and deadline handling.
  Use a fake debug transport and the same `BareHandle`/`FakeBackend` patterns.
- Symbol services are testable with synthetic module lists and a local PDB fixture; never require
  the network in a test.
- `tests/CMakeLists.txt` must not link `mpfshost_host`, and the test binary must not need
  `vmm.so` or `leechcore.so`.
- A real watchpoint can only be tested against a live guest. Keep that out of the unit suite;
  document a manual procedure (stop, arm, trigger, assert hit, resume, verify `query-status`).

## 8. Open questions

- Should QMP access live in the host (privileged) or be a client-side feature? The host already
  has the MemProcFS handles and the access policy, but QMP is a separate trust boundary.
- How should `cr3 → pid` be implemented? MemProcFS exposes process information, but the exact
  API and whether it is cheap enough per hit need investigation.
- Process-scoped watchpoints: filter on CR3 after the hit, or use a per-vCPU context switch
  (write CR3 / select thread) before arming? The former is simpler but reports other processes
  first; the latter is fragile under SMP.
- What is the PDB policy? Prefer a local cache, make downloads explicit, and fail closed when
  offline.
- How to handle multiple simultaneous watchpoints with only four debug registers, and what to do
  when the guest already uses them.
- Does `pdb_type_*` plus `read_memory` give enough structure reconstruction to avoid a
  disassembler, or is a small decoder needed?
- Windows-specific: does an attached GDB stub interact badly with HVCI, PatchGuard, or a guest
  kernel debugger? This session did not test those.

## Appendix A: reproduction notes

The commands below are the shape of what worked. Replace pids, addresses and the watch value.

```text
# mpfshost
search_memory  target=windows pid=<pid> hex_pattern=<utf16 bytes>
read_memory    target=windows pid=<pid> address=<addr> size=...

# QMP / HMP
stop
gdbserver tcp:127.0.0.1:1234
# ... gdb: target remote; watch *(unsigned short*)<addr>; continue
# trigger the write
gdbserver none
cont
```

```text
# gdb, the part that reliably captured the hit
set pagination off
set confirm off
target remote 127.0.0.1:1234
watch *(unsigned short*)<addr>
continue
```

After the hit:

```text
# mpfshost
list_modules  target=windows pid=<pid> counts=true
list_exports  target=windows pid=<pid> module=<module> limit=1000
pdb_symbol_name target=windows pid=<pid> module=<module> address=<module offset>
```

## Appendix B: observations worth keeping

- `list_exports` returns `va_function`; `function` is the decorated name.
- `list_modules` needs `counts=true` to include `image_size` in the result used here.
- `search_memory` accepts either `pattern` or `hex_pattern`, not both, and caps both at 32 bytes.
- `write_memory` takes a hex string; equal-length replacement matters when writing into an edit
  control, because its internal length field is separate from the bytes.
- `query-display-options` returning `type: none` is a strong hint that `screendump` will not show
  the guest desktop.
- The MCP server process must be restarted after upgrading the binary; the tool catalogue is
  fixed for the life of the process. The harness saw 46 tools before the restart and 50 after.

## Appendix C: artifacts

Throwaway scripts from the session (`watch*.gdb`, QMP helpers) were kept in `tmp/` and are not
committed. The reproduction notes above are sufficient to recreate them.
