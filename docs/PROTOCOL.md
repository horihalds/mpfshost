# mpfshost wire protocol

Read this before you add or change a message type, touch `src/core/ipc/`, a `Session::handle*` method, a
`src/core/client/client_*.cpp` call, or anything that sends frames. `src/core/ipc/frame.hpp` is the source of truth;
this document explains it.

## Transport

- `AF_UNIX` / `SOCK_STREAM`, default path `/run/mpfshost.sock` (`--socket`).
- One connection serves one client. Requests on a connection are handled strictly one at a time
  (`Session::run` reads a frame, dispatches it, and only then reads the next one).
- A connection binds to at most one VM target. There is no unbind; reconnect to switch targets.
- The client library (`src/core/client/client_*.cpp`) opens a fresh connection per call and, for target-scoped calls,
  sends `BindRequest` first (`connectAndBind`).

## Frame header

20 bytes, little-endian, written by `encodeFrame` (`src/core/ipc/frame.cpp`):

 Offset | Size | Field | Value |
--------|------|-------|-------|
 0 | u32 | magic | `kProtocolMagic` = `0x5350464D` (`'M','F','P','S'`) |
 4 | u16 | version | `kProtocolVersion` = `1` |
 6 | u16 | type | `MessageType` |
 8 | u64 | request id | chosen by the client, echoed in every response frame |
 16 | u32 | payload length | at most `kMaxPayloadLength` = 1 MiB |

`readFrame` returns `Complete`, `Rejected` (bad magic, version or length), `Timeout` or `Closed`. A `Rejected` frame
gets one `ErrorResponse` (`UnsupportedVersion` when the version differs, otherwise `MalformedPayload`) and the
session ends. `Timeout` (idle timeout, `SO_RCVTIMEO`) and `Closed` end the session silently.

## Payload encoding

- Integers: `ByteWriter::writeU8/U16/U32/U64`, little-endian, fixed width.
- Strings: `u32` byte length, then UTF-8 bytes (`writeString` / `readString`).
- Byte blobs: `u32` length, then bytes (`writeBytes` / `readBytes`).
- Every `decode*` function reads fields in order and returns `std::nullopt` when `!reader.ok() || !reader.atEnd()`.
  Trailing bytes are an error. Keep this check in every new decoder.
- `*Data` payloads are a `u32` entry count followed by the entries. Decoders reject `count > payload.size()` before
  reserving.
- `*End` payloads start with `u8 status`, then the delivered entry count (`bytesDelivered` for byte streams), the
  family-specific totals and flags (`truncated`, `mapEntries`, …) and a trailing `string error`. Check the concrete
  struct in `src/core/ipc/message.hpp`.
- Descriptor (entry) codecs are the `write<Descriptor>` / `read<Descriptor>` helpers in
  `src/core/ipc/message_codec.hpp` (namespace `mpfshost::detail`, not installed). Strings inside entries are read with
  `readCappedString` (`kMaxMapStringLength` = 1024).

## Response framing rules

These rules are what clients rely on. Breaking them desynchronises the stream.

1. **Single-frame request:** answered by exactly one frame of its response type, or by one `ErrorResponse`.
2. **Streamed request:** answered by zero or more `*Data` frames followed by exactly one `*End` frame, or by one
   `ErrorResponse` **instead of** any data. Every frame echoes the request id.
3. **Never send an `ErrorResponse` after a data frame of the same request.** If a request must fail after it
   streamed, the session ends the connection (`_writeFailed = true`) instead. `Session::abortRequest` and
   `Session::sendFrame` enforce this; use them, never `writeFrame` directly.
4. A single-frame response larger than `kMaxPayloadLength` is replaced by `ErrorResponse(ResultTooLarge)`
   (`Session::sendPayload`).
5. Unknown types get `ErrorResponse(UnknownMessage)` from the `default:` branch of `Session::dispatch`.
6. An `ErrorResponse` payload is `u16 code` + `string message`.

The client side checks rule 2: `exchangeStreamed` collects data frames until the expected `*End` type, and each call
verifies the `*End` status and that the number of delivered entries equals the count in the `*End` frame.

## Message types

Requests are `0x0001`–`0x004D`; responses are `0x8001`–`0x8071` plus `ErrorResponse` = `0x8FFF`. Values
`0x8030`–`0x8035` are unused; do not reuse them silently, and always take the next free value at the end of each
range.

 Request | Value | Answer | Mode | CLI command |
---------|-------|--------|------|-------------|
 `ListTargetsRequest` | `0x0001` | `ListTargetsResponse` `0x8001` | single | `list-targets` |
 `BindRequest` | `0x0002` | `BindResponse` `0x8002` | single | `bind` |
 `ProcessListRequest` | `0x0003` | `ProcessListResponse` `0x8003` | single | `processes` |
 `MemoryReadRequest` | `0x0004` | `MemoryReadData` `0x8004` / `MemoryReadEnd` `0x8005` | stream | `read` |
 `MemoryWriteRequest` | `0x0005` | `MemoryWriteResponse` `0x8006` | single | `write` |
 `ScatterCreateRequest` | `0x0006` | `ScatterCreateResponse` `0x8007` | single | `scatter` |
 `ScatterPrepareRequest` | `0x0007` | `ScatterPrepareResponse` `0x8008` | single | `scatter` |
 `ScatterExecuteRequest` | `0x0008` | `ScatterExecuteData` `0x8009` / `ScatterExecuteEnd` `0x800A` | stream | `scatter` |
 `ScatterCloseRequest` | `0x0009` | `ScatterCloseResponse` `0x800B` | single | `scatter` |
 `ModuleListRequest` | `0x000A` | `ModuleListData` `0x800C` / `ModuleListEnd` `0x800D` | stream | `modules` |
 `MemoryMapRequest` | `0x000B` | `MemoryMapData` `0x800E` / `MemoryMapEnd` `0x800F` | stream | `maps` |
 `ThreadListRequest` | `0x000C` | `ThreadListData` `0x8010` / `ThreadListEnd` `0x8011` | stream | `threads` |
 `VmListRequest` | `0x000D` | `VmListResponse` `0x8012` | single | `vm-list` |
 `PhysicalReadRequest` | `0x000E` | `PhysicalReadData` `0x8013` / `PhysicalReadEnd` `0x8014` | stream | `phys-read` |
 `PhysicalWriteRequest` | `0x000F` | `PhysicalWriteResponse` `0x8015` | single | `phys-write` |
 `GpaTranslateRequest` | `0x0010` | `GpaTranslateResponse` `0x8016` | single | `vm-translate` |
 `Virt2PhysRequest` | `0x0011` | `Virt2PhysResponse` `0x8017` | single | `virt2phys` |
 `PhysicalScatterRequest` | `0x0012` | `PhysicalScatterData` `0x8018` / `PhysicalScatterEnd` `0x8019` | stream | `phys-scatter` |
 `VfsListRequest` | `0x0013` | `VfsListData` `0x801A` / `VfsListEnd` `0x801B` | stream | `vfs-list` |
 `VfsReadRequest` | `0x0014` | `VfsReadData` `0x801C` / `VfsReadEnd` `0x801D` | stream | `vfs-read` |
 `ProcessInfoRequest` | `0x0015` | `ProcessInfoResponse` `0x801E` | single | `proc-info` |
 `SymbolLookupRequest` | `0x0016` | `SymbolLookupResponse` `0x801F` | single | `symbol` |
 `ModuleLayoutRequest` | `0x0017` | `ModuleLayoutResponse` `0x8020` | single | `sections` |
 `CancelRequest` | `0x0018` | `CancelResponse` `0x8021` | single | — |
 `VirtualQueryRequest` | `0x0019` | `VirtualQueryResponse` `0x8022` | single | `vquery` |
 `MemorySearchRequest` | `0x001A` | `MemorySearchData` `0x8023` / `MemorySearchEnd` `0x8024` | stream | `search` |
 `PageReadRequest` | `0x001B` | `PageReadData` `0x8025` / `PageReadEnd` `0x8026` | stream | `page` |
 `PrefetchRequest` | `0x001C` | `PrefetchResponse` `0x8027` | single | `prefetch` |
 `HandleListRequest` | `0x001D` | `HandleListData` `0x8028` / `HandleListEnd` `0x8029` | stream | `handles` |
 `ExportListRequest` | `0x001E` | `ExportListData` `0x802A` / `ExportListEnd` `0x802B` | stream | `eat` |
 `ImportListRequest` | `0x001F` | `ImportListData` `0x802C` / `ImportListEnd` `0x802D` | stream | `iat` |
 `UnloadedModuleListRequest` | `0x0020` | `UnloadedModuleListData` `0x802E` / `UnloadedModuleListEnd` `0x802F` | stream | `unloaded` |
 `ServiceListRequest` | `0x0021` | `ServiceListData` `0x8036` / `ServiceListEnd` `0x8037` | stream | `services` |
 `UserListRequest` | `0x0022` | `UserListData` `0x8038` / `UserListEnd` `0x8039` | stream | `users` |
 `NetworkListRequest` | `0x0023` | `NetworkListData` `0x803A` / `NetworkListEnd` `0x803B` | stream | `net` |
 `HeapListRequest` | `0x0024` | `HeapListData` `0x803C` / `HeapListEnd` `0x803D` | stream | `heap` |
 `HeapAllocListRequest` | `0x0025` | `HeapAllocListData` `0x803E` / `HeapAllocListEnd` `0x803F` | stream | `heap-alloc` |
 `KDriverListRequest` | `0x0026` | `KDriverListData` `0x8040` / `KDriverListEnd` `0x8041` | stream | `kdriver` |
 `KDeviceListRequest` | `0x0027` | `KDeviceListData` `0x8042` / `KDeviceListEnd` `0x8043` | stream | `kdevice` |
 `KObjectListRequest` | `0x0028` | `KObjectListData` `0x8044` / `KObjectListEnd` `0x8045` | stream | `kobject` |
 `PteListRequest` | `0x0029` | `PteListData` `0x8046` / `PteListEnd` `0x8047` | stream | `pte` |
 `PoolListRequest` | `0x002A` | `PoolListData` `0x8048` / `PoolListEnd` `0x8049` | stream | `pool` |
 `PfnListRequest` | `0x002B` | `PfnListData` `0x804A` / `PfnListEnd` `0x804B` | stream | `pfn` |
 `CapabilitiesRequest` | `0x002C` | `CapabilitiesResponse` `0x804C` | single | `capabilities` |
 `PdbLoadRequest` | `0x002D` | `PdbLoadResponse` `0x804D` | single | `pdb-load` |
 `PdbSymbolNameRequest` | `0x002E` | `PdbSymbolNameResponse` `0x804E` | single | `pdb-symbol` |
 `PdbSymbolAddressRequest` | `0x002F` | `PdbSymbolAddressResponse` `0x804F` | single | `pdb-address` |
 `PdbTypeSizeRequest` | `0x0030` | `PdbTypeSizeResponse` `0x8050` | single | `pdb-type-size` |
 `PdbTypeChildOffsetRequest` | `0x0031` | `PdbTypeChildOffsetResponse` `0x8051` | single | `pdb-type-child` |
 `ProcessListAllRequest` | `0x0032` | `ProcessListAllResponse` `0x8052` | single | `processes-all` |
 `VadExListRequest` | `0x0033` | `VadExListData` `0x8053` / `VadExListEnd` `0x8054` | stream | `vadex` |
 `ScatterExecuteReadRequest` | `0x0034` | `ScatterExecuteReadData` `0x8055` / `ScatterExecuteReadEnd` `0x8056` | stream | `scatter --repeat` |
 `ScatterClearRequest` | `0x0035` | `ScatterClearResponse` `0x8057` | single | `scatter --clear` |
 `PoolTagListRequest` | `0x0036` | `PoolTagListData` `0x8058` / `PoolTagListEnd` `0x8059` | stream | `pool-tags` |
 `ConfigSetRequest` | `0x0037` | `ConfigSetResponse` `0x805A` | single | `config-set` |
 `VfsWriteRequest` | `0x0038` | `VfsWriteResponse` `0x805B` | single | `vfs-write` |
 `ModuleByAddressRequest` | `0x0039` | `ModuleByAddressResponse` `0x805C` | single | `module-at` |
 `ProcessByDtbRequest` | `0x003A` | `ProcessByDtbResponse` `0x805D` | single | `process-for-dtb` |
 `DebugStateRequest` | `0x003B` | `DebugStateResponse` `0x805E` | single | `debug-status` |
 `DebugControlRequest` | `0x003C` | `DebugControlResponse` `0x805F` | single | `debug-stop` / `debug-continue` |
 `DebugRegistersRequest` | `0x003D` | `DebugRegistersResponse` `0x8060` | single | `debug-registers` |
 `DebugThreadsRequest` | `0x004B` | `DebugThreadsResponse` `0x806F` | single | `debug-threads` |
 `DisassembleRequest` | `0x004C` | `DisassembleResponse` `0x8070` | single | `disassemble` |
 `DebugBacktraceRequest` | `0x004D` | `DebugBacktraceResponse` `0x8071` | single | `debug-backtrace` |
 `DebugWatchRequest` | `0x003E` | `DebugWatchData` `0x8061` / `DebugWatchEnd` `0x8062` | stream | `debug-watch` |
 `DebugBreakpointRequest` | `0x003F` | `DebugBreakpointResponse` `0x8063` | single | `debug-break` |
 `DebugStepRequest` | `0x0040` | `DebugStepResponse` `0x8064` | single | `debug-step` |
 `DebugDetachRequest` | `0x0041` | `DebugDetachResponse` `0x8065` | single | `debug-detach` |
 `DebugAttachRequest` | `0x0042` | `DebugAttachResponse` `0x8066` | single | `debug-attach` |
 `DebugBreakpointArmRequest` | `0x0043` | `DebugBreakpointArmResponse` `0x8067` | single | `debug-arm-break` |
 `DebugBreakpointRemoveRequest` | `0x0044` | `DebugResourceRemoveResponse` `0x8068` | single | `debug-remove-break` |
 `DebugBreakpointListRequest` | `0x0045` | `DebugBreakpointListResponse` `0x8069` | single | `debug-breakpoints` |
 `DebugWatchArmRequest` | `0x0046` | `DebugWatchArmResponse` `0x806A` | single | `debug-arm-watch` |
 `DebugWatchRemoveRequest` | `0x0047` | `DebugResourceRemoveResponse` `0x8068` | single | `debug-remove-watch` |
 `DebugWatchListRequest` | `0x0048` | `DebugWatchListResponse` `0x806B` | single | `debug-watchpoints` |
 `DebugContinueRequest` | `0x0049` | `DebugContinueResponse` `0x806C` | single | `debug-resume` |
 `DebugWaitRequest` | `0x004A` | `DebugWaitHit` `0x806D` / `DebugWaitEnd` `0x806E` | stream | `debug-wait` |

## Address lookups

`ModuleByAddressRequest` carries a pid and a virtual address. The host returns the module whose half-open image range
contains the address, plus the module-relative offset; an address outside every module is `NotFound`. The client-side
`resolve` command and MCP tool compose this lookup with `ExportListRequest` and `PdbSymbolNameRequest` in one
convenience call; they use no additional message types.
`ProcessByDtbRequest` carries a CR3 or directory table base. The low twelve CR3 flag bits are masked off before the
comparison, and both `paDTB` and `paDTB_UserOpt` are candidates, so a KPTI user DTB resolves to the same process;
`matchedUser` says which one matched. The host caches module lists per pid and one process/DTB snapshot for one second;
a miss from a snapshot older than 250 ms is retried once before it is reported as `NotFound`.

## Disassembly

`DisassembleRequest` asks the host to read process memory at `address` and decode instructions. It does not
touch the GDB stub or the guest's run state, so it works while the guest is running.

 Field | Type | Meaning |
-------|------|---------|
 `pid` | `u32` | Process whose address space is read; zero is `MalformedPayload` |
 `address` | `u64` | Virtual address of the first instruction |
 `count` | `u32` | Instructions to decode, 1–`kMaxDisassembleInstructions` (128); zero is `MalformedPayload` |

The host reads at most `count * 15` bytes, capped at `kMaxDisassembleBytes` (1920), through its ordinary process
memory path, so a request never causes an unbounded read. The decoder is x86-64 only. An encoding it cannot decode is
reported as one `.byte 0xNN` record and consumes exactly one byte rather than guessing a length.

`DisassembleResponse` is a `u32` count followed by records:

 Field | Type | Meaning |
-------|------|---------|
 `address` | `u64` | Address of this instruction |
 `bytes` | bytes | Up to 15 instruction bytes |
 `text` | string | Intel-syntax text, or `.byte 0xNN` for an unknown encoding |

## Debug control

`DebugStateRequest` has no payload and asks the host for the guest's current control state. `DebugControlRequest`
carries one `u8` action: `0` stop, `1` continue; any other value is `MalformedPayload`. Both are answered by the same
`DebugStatus` record:

 Field | Type | Meaning |
-------|------|---------|
 `runState` | `u8` | `0` unknown, `1` running, `2` paused, `3` shutdown, `4` suspended, `5` guest-panicked, `6` other QMP state |
 `stubActive` | `u8` | `1` when a GDB stub is active |
 `hostPaused` | `u8` | `1` when this host pressed stop and has not continued the VM |
 `vcpuCount` | `u32` | vCPU count from QMP `query-cpus-fast` |
 `watchCount` | `u32` | watchpoints armed by the host |
 `stubOwned` | `u8` | `1` when this host process started the active stub |
 `foreignStub` | `u8` | `1` when a stub is active but this host process does not own it |
 `architecture` | string | Guest architecture from QMP `query-cpus-fast` (`x86_64`, `aarch64`, …), or `unknown` |

The debugger only supports x86-64 guests. `debug-registers`, `debug-watch`, `debug-break` and `debug-step` ask QMP
for the architecture first and answer `CapabilityUnavailable` with the detected name when it is not `x86_64`, before
starting a GDB stub. When `query-cpus-fast` omits both `target` and `qom-type`, the host starts a stub and falls back
to the register names in `target.xml`; an aarch64 or 32-bit description, or one that names no known architecture, is
rejected the same way before any register or debug resource is touched. `debug-status` remains available because it
only needs QMP, and reports the architecture it detected. A future generic register form would need a per-architecture
register layout, a decoder that is not x86-specific, and architecture-aware `rip`/`cr3` equivalents instead of the
current fixed x86-64 record.

The host reaches the target's QEMU control plane through the QMP Unix socket that the target scan pairs with the QEMU
pid (`list-targets` reports it). No new host flag gates these types; the connection's access policy is the gate, as
for memory writes. The host keeps one QMP connection per target open across requests and reconnects when the
transport closes or a reply desynchronizes the stream; QMP commands are serialized without blocking status or
control during a watch or breakpoint. Stop and continue are **destructive**: a paused guest stops executing until it
is continued. The host only reports `hostPaused = 1` when its own `debug-stop` performed the pause; it still honours
`debug-continue` for a guest paused by another client. `CapabilityUnavailable` means the bound handle does not
implement debug control or QMP could not be reached (a missing socket or a failed connect); `Internal` means a QMP
command really failed.

## Debug attach

`DebugAttachRequest` has no payload. The requesting connection becomes the owner of one persistent GDB stub. The
host refuses a second attach with `Busy` while an attachment exists, and refuses a stale stub reported by
`info chardev` with `InvalidState`; the error names `debug-detach` as the recovery command. A guest that is running
is paused for the attachment and reported as `guestPaused`.

`DebugAttachResponse` starts with the `DebugStatus` record and appends:

 Field | Type | Meaning |
-------|------|---------|
 `status` | `DebugStatus` | Run state and bookkeeping after the attach |
 `stubMode` | string | `unix` or `tcp`, the stub endpoint kind |
 `guestPaused` | `u8` | `1` when this request paused a running guest |

While attached, `debug-registers`, `debug-watch`, `debug-break` and `debug-step` reuse the attached stub instead of
starting and stopping their own. They still run one at a time. The attachment ends on `debug-detach`, when the owning
session disconnects, on cancellation of a watch or breakpoint owned by that session, or on host shutdown.

## Debug detach

`DebugDetachRequest` has no payload. It is the explicit recovery path for a stub left behind by an earlier host
process, and it also ends an attachment. The host asks QEMU for `info chardev`, stops any active stub with
`gdbserver none` (including a stub it did not start), removes its stub socket file, and clears the attachment and
watch bookkeeping. It continues the guest only when the attachment paused it, or, when there was no attachment, when
this host process performed the pause (`hostPaused`). A guest left stopped by a foreign stub stays stopped and is
reported as such; `debug-continue` resumes it.

`DebugDetachResponse` starts with the same `DebugStatus` record and appends four `u8` flags:

 Field | Type | Meaning |
-------|------|---------|
 `status` | `DebugStatus` | Run state and bookkeeping after the detach |
 `stubStopped` | `u8` | `1` when this request stopped an active stub |
 `foreignStub` | `u8` | `1` when the stopped stub was not started by this host |
 `guestResumed` | `u8` | `1` when this request continued the guest |
 `detached` | `u8` | `1` when this request ended an attachment |

The host performs the same teardown on a normal shutdown: it stops a stub it owns, removes its socket file, and
resumes a guest it paused. It never stops a foreign stub during shutdown; use `debug-detach` for that.

## Persistent debug resources

A resource armed with `DebugBreakpointArmRequest` or `DebugWatchArmRequest` stays on the attached stub until it is
removed, the attachment ends, the host shuts down, or a snapshot/reset removes the stub. Both arm requests require an
attachment to exist and the guest to be paused; otherwise they answer `InvalidState`. Any client bound to the target
may manage resources while the attachment exists, the same way register reads work; only the owner session ends the
attachment when it disconnects. Every resource gets a `u32` id from a counter that is never reused, so ids stay stable
while other resources are removed.

`DebugBreakpointArmRequest` carries `pid` (`u32`), `address` (`u64`) and `kind` (`u8`: `0` software, `1` hardware).
`DebugWatchArmRequest` carries `pid`, `address`, `width` (`u32`, 1, 2, 4 or 8) and `access` (`u8`, only `0` write).
The responses carry the assigned `id`, the original fields, and a `slot` (`u32`): the hardware debug slot index
0–3, or `0xFFFFFFFF` for a software breakpoint. At most four hardware slots exist and hardware breakpoints and
watchpoints share them, so arming a fifth answers `InvalidState`. Software breakpoints consume no slot.

A software breakpoint on a user-mode address is armed from a vCPU that is already in the target process, because
QEMU applies the instruction patch through the selected vCPU's address space. If no stub thread has the target
process DTB or `dtbUser`, arming answers `InvalidState` and names the limitation instead of forwarding QEMU's raw
`E22`. Kernel addresses and hardware breakpoints keep selecting the first stub thread.

`DebugBreakpointRemoveRequest` and `DebugWatchRemoveRequest` carry one `u32` id and share
`DebugResourceRemoveResponse` (`u32 id`, `u8 removed`); an unknown id is not an error and reports `removed = 0`.
`DebugBreakpointListRequest` and `DebugWatchListRequest` have no payload and answer a `u32` count followed by the
same records the arm responses use.

`DebugContinueRequest` has no payload and resumes the attached stub without removing anything; it answers the usual
`DebugStatus` record. `DebugWaitRequest` carries `timeoutMs` (`u32`, 1–`kMaxDebugWatchTimeoutMs`) and `maxHits`
(`u32`, 1–`kMaxDebugWaitHits` 64). It resumes a stopped guest, waits for a stop from any armed resource, and streams
zero or more `DebugWaitHit` records followed by exactly one `DebugWaitEnd`, or one `ErrorResponse` instead of all of
them. The guest is left stopped at the last reported hit, so a later `DebugWaitRequest` continues to the next one.

A `DebugWaitHit` carries `id` (`u32`), `kind` (`u8`: `0` breakpoint, `1` watchpoint), `breakpointKind` (`u8`),
`address` (`u64`), one fixed `DebugRegisterRecord`, then `module`, `moduleOffset`, the export fields and the PDB
symbol fields with the same layout as `DebugBreakpointResponse` below, and finally `unverified` (`u8`). A hardware
breakpoint whose stop CR3 matches neither process DTB is accepted through the target's module map only when the
address lies in a target module, and then `unverified = 1`; a stop at the same PC outside those modules belongs to
another process and is skipped. Watchpoints keep their watch-address attribution. `DebugWaitEnd` uses the
`DebugWatchEnd` layout: status (`0` completed, `1` timeout, `2` cancelled, `3` error), hit count, skipped count and
an error string.
A stop that matches no armed resource is skipped and counted; a stub close, guest exit or snapshot while waiting
clears the resources and ends as `Error`.

## Debug registers

`DebugRegistersRequest` carries one `u32` vCPU index, or `0xFFFFFFFF` for every vCPU. An index at or above
`kMaxDebugVcpuRecords` (256) is `MalformedPayload`. The request is only valid while the guest is paused and no other
stub owns the debug channel; otherwise the host answers `InvalidState` (`20`). The host leaves the run state
unchanged: it starts a stub on a Unix chardev under `/tmp` (or reuses an attached one), reads the registers, and stops
its own stub and re-checks that the guest is still paused.

`DebugRegistersResponse` is a `u32` record count followed by fixed x86-64 records:

 Field | Type | Meaning |
-------|------|---------|
 `vcpu` | `u32` | 0-based position in the stub's thread list |
 `threadId` | string | GDB remote-protocol thread id |
 `rip`, `rsp`, `rbp`, `eflags` | `u64` | Instruction and stack pointers and flags |
 `cr0`, `cr3`, `cr4`, `cr8` | `u64` | Control registers |
 `rax` … `r15`, except `rsp`/`rbp` | `u64` | General registers (`rax`, `rbx`, `rcx`, `rdx`, `rsi`, `rdi`, `r8`–`r15`) |
 `guestPid` | `u32` | Guest process that owns `cr3`, zero when no process matches |
 `guestTid` | `u32` | Guest thread whose stack contains `rsp`, zero when unknown |
 `processFound`, `threadFound` | `u8` | Whether `guestPid` and `guestTid` are valid |

The guest context fields are derived the same way as in `DebugThreadsResponse`: the CR3 selects a process, then the
stack pointer selects one of its threads. They are informational and are reported on every register record, watch
hit, breakpoint hit and wait hit.

The host reads QEMU's `target.xml` (following `xi:include`), uses its register numbering and byte layout for the
`g` packet, and falls back to a fixed x86-64 layout only for a register that the XML omits. If the stub cannot be
started it tries loopback TCP before failing; it never uses the all-interfaces default and never stops a stub it did
not start. The record layout above and the register names in every request are x86-64; an unsupported architecture is
rejected as described under "Debug control".

## Debug threads

`DebugThreadsRequest` has no payload. It lists the same stub threads `DebugRegistersRequest` would return, but with
only the context needed to choose one: the vCPU index, GDB thread id, `rip`, `rsp` and `cr3`. It has the same paused,
architecture and stub-ownership rules as `debug-registers`, starts a temporary stub when no attachment exists, and
stops that stub before answering. An attached stub is reused and left in place.

`DebugThreadsResponse` is a `u32` count followed by records:

 Field | Type | Meaning |
-------|------|---------|
 `vcpu` | `u32` | 0-based position in the stub's thread list |
 `threadId` | string | GDB remote-protocol thread id |
 `rip`, `rsp`, `cr3` | `u64` | Registers captured for that thread |
 `processFound` | `u8` | The CR3 matched a known process |
 `pid` | `u32` | Guest process id when `processFound`, otherwise zero |
 `threadFound` | `u8` | The stack pointer matched one of that process's threads |
 `tid` | `u32` | Guest thread id when `threadFound`, otherwise zero |

The vCPU index is a position in the stub's thread list and is what `--vcpu` selects. The GDB thread id is the
stable name QEMU prints (for example `p01.03`) and is what `DebugStepRequest.threadId` selects. The guest pid/tid are
resolved through the CR3 and the process thread stack ranges; they are informational and do not change which thread
a request operates on.

## Debug backtrace

`DebugBacktraceRequest` reads one stopped thread's registers and walks its frame-pointer chain. It requires a paused
guest and follows the same architecture and stub-ownership rules as `debug-registers`; an attached stub is reused,
otherwise a temporary stub is started and stopped before answering.

 Field | Type | Meaning |
-------|------|---------|
 `pid` | `u32` | Process used to read the stack and symbolize frames; zero is `MalformedPayload` |
 `vcpu` | `u32` | 0-based stub thread index; ignored when `threadId` is set |
 `threadId` | string | Optional GDB thread id; when set it selects the thread and an unknown id is `InvalidState` |
 `maxFrames` | `u32` | Frame limit, 1–`kMaxDebugBacktraceFrames` (128); zero is `MalformedPayload` |

The first frame is the selected thread's `rip` with its current `rbp`. For each frame the host reads the saved frame
pointer and return address at `[rbp]` and `[rbp+8]` from the requested process. A null saved frame pointer is the
normal end of the chain; the return address read from that frame is still reported, then the walk stops. A nonzero
saved pointer that does not advance, is not 8-byte aligned, or lies more than
`kMaxDebugBacktraceDepthBytes` (1 MiB) above the starting `rsp` is corrupt and is not followed. A failed stack read
also ends the walk. Every read is therefore bounded, and a corrupt chain cannot loop or run away.

Each frame is symbolized through the same module, nearest-export and already-loaded-PDB path as a watch hit.
Symbolization is best effort: a frame outside every module, or with no matching export or PDB symbol, keeps its raw
`address` with the empty symbol fields.

`DebugBacktraceResponse` is a `u32` frame count, the frames, a `u8 truncated` flag and a trailing `string stopReason`:

 Field | Type | Meaning |
-------|------|---------|
 `address` | `u64` | Instruction pointer for the frame |
 `framePointer` | `u64` | `rbp` value used to reach the next frame |
 `module` | string | Module containing `address`, empty when none |
 `moduleOffset` | `u64` | `address - module base` |
 `exportFound`, `exportAddress`, `exportOffset`, `exportName` | `u8`, `u64`, `u64`, string | Nearest export at or below the address |
 `pdbFound`, `pdbSymbol`, `pdbDisplacement` | `u8`, string, `u32` | Symbol from an already-loaded PDB, when present |

`truncated` is false only when the chain ended at a null saved frame pointer. It is true when the frame limit, the
depth limit, an unreadable frame, a zero return address or a corrupt saved pointer stopped the walk, and
`stopReason` explains which.

## Debug watch

`DebugWatchRequest` is streamed: zero or more `DebugWatchData` frames followed by exactly one `DebugWatchEnd`, or one
`ErrorResponse` instead. A dedicated connection per watch is expected, so closing it is the cancellation signal.

 Field | Type | Meaning |
 -------|------|---------|
 `pid` | `u32` | Process whose address is watched; zero is `MalformedPayload` |
 `address` | `u64` | Watched linear address, or an offset from the module or symbol named below; zero is `MalformedPayload` only for a raw address |
 `width` | `u32` | Watch width in bytes: 1, 2, 4 or 8 |
 `access` | `u8` | `0` write; other values are `MalformedPayload` in this slice |
 `maxHits` | `u32` | Stop after this many matching hits, 1–`kMaxDebugWatchHits` (64) |
 `timeoutMs` | `u32` | Wall-clock wait, 1–`kMaxDebugWatchTimeoutMs` (3 600 000) |
 `module` | string | Optional module name; when set, `address` is an offset from this module or from `symbol` |
 `symbol` | string | Optional export or PDB symbol in `module`; empty means `address` is module-relative |

The CLI and MCP layers accept `module!symbol`, `module!symbol+offset`, `module!+offset`, `module+offset` and a bare
`module` name (its base) as `--address`/`address`, and parse them into these two fields. A symbol expression is
resolved on the host before any stub is started. Exports are preferred over PDB symbols with the same name, an
ordinal-only export is not addressable by name, and a module that matches several loaded images or a symbol that
matches nothing is answered with `NotFound` (`16`), not `Internal`. Raw hex addresses keep the old behaviour and are
stored in `address` with both string fields empty.

The host pauses the guest if it is running, starts a stub it owns, reads the process DTBs (`paDTB` and
`paDTB_UserOpt`), arms one `Z2` watchpoint (QEMU's debug registers are global, not per vCPU), resumes execution
through the stub and polls with short timeouts so cancellation, the request deadline and peer loss are observed. Every stop reply is matched by masking the
low twelve bits of `cr3` and comparing it with both process DTBs; a mismatch resumes the guest, counts one skipped hit
and keeps watching. On a match the host captures `rip`, `rsp` and `cr3` before reading the watched value from the
requested process and resolving `rip` against the module list, the nearest export and any already-loaded PDB (no
download is started).

`DebugWatchData` fields:

 Field | Type | Meaning |
 -------|------|---------|
 `hitIndex` | `u32` | 0-based matching-hit index |
 `vcpu` | `u32` | 0-based position in the stub's thread list |
 `threadId` | string | GDB remote-protocol thread id |
 `rip`, `rsp`, `cr3` | `u64` | Registers captured at the stop |
 `address` | `u64` | Watched linear address |
 `oldValue`, `newValue` | `u64` | Previous and current watch value, little-endian |
 `bytesBefore` | bytes | Up to 16 bytes ending at `rip`; empty only when the read failed or `rip` is zero. The client decodes the last store from these bytes when it can do so unambiguously. |
 `module` | string | Module containing `rip`, empty when none |
 `moduleOffset` | `u64` | `rip - module base` |
 `exportFound`, `exportAddress`, `exportOffset`, `exportName` | `u8`, `u64`, `u64`, string | Nearest export at or below `rip` |
 `pdbFound`, `pdbSymbol`, `pdbDisplacement` | `u8`, string, `u32` | Symbol from an already-loaded PDB, when present |
 `guestPid`, `guestTid`, `processFound`, `threadFound` | `u32`, `u32`, `u8`, `u8` | Guest context of the stop, as in `DebugRegisterRecord` |

`DebugWatchEnd` carries a `u8` status (`0` completed, `1` timeout, `2` cancelled, `3` error), the `u32` hit count, the
`u32` skipped-hit count and an error string. Teardown removes the watchpoints, stops a stub the host started, resumes
the guest if this request paused it (an already-paused guest is left paused) and verifies the result with
`query-status`. `Busy` (`19`) means another exclusive debug operation (registers, watch, breakpoint, step or detach)
holds the target; `InvalidState` means another owner holds the GDB stub. `debug-status` and
`debug-stop`/`debug-continue` are not exclusive and stay available while a watch is in flight.

## Debug breakpoints

`DebugBreakpointRequest` is a one-shot request. It arms one breakpoint, resumes the guest, and answers with exactly
one `DebugBreakpointResponse` after the first matching hit, a timeout, a cancellation or an error. The breakpoint is
always removed during teardown, so a caller that wants to keep stopping repeats the request.

 Field | Type | Meaning |
-------|------|---------|
 `pid` | `u32` | Process used to scope and symbolize the hit; zero is `MalformedPayload` |
 `address` | `u64` | Breakpoint linear address, or an offset from the module or symbol named below; zero is `MalformedPayload` only for a raw address |
 `kind` | `u8` | `0` software (`Z0`), `1` hardware (`Z1`); other values are `MalformedPayload` |
 `timeoutMs` | `u32` | Wall-clock wait, 1–`kMaxDebugWatchTimeoutMs` (3 600 000) |
 `module` | string | Optional module name; when set, `address` is an offset from this module or from `symbol` |
 `symbol` | string | Optional export or PDB symbol in `module`; empty means `address` is module-relative |

Symbolic `address` values are accepted in the same forms as `DebugWatchRequest` and are resolved before the stub is
armed. A missing or ambiguous module/symbol is `NotFound`. The resolved absolute address is returned in
`DebugBreakpointResponse.address` and in every `DebugWatchData.address`.

The host pauses the guest if needed, starts a stub it owns, reads the process DTBs, selects one thread and arms the
breakpoint once (QEMU's debug registers are global). A software breakpoint on a user-mode address selects a thread
whose CR3 is the target `dtb` or `dtbUser`; when no stub thread is in the target process the request answers
`InvalidState` rather than letting QEMU reject the patch with `E22`. It then resumes execution and polls with short
timeouts so cancellation, the request deadline and peer loss are observed. The stop PC must equal the requested
address. A stop reply whose CR3 matches either process DTB is a verified hit. A hardware breakpoint on a shared user
address traps in every process that maps it, so a CR3 mismatch is accepted only when the address lies inside one of
the target's modules — the case where a user CR3 is a shadow table MemProcFS does not report — and that hit reports
`unverified = 1`. Any other mismatch is skipped and execution resumes.

`DebugBreakpointResponse` carries a `u8` status (`0` hit, `1` timeout, `2` cancelled, `3` error), the `u32` skipped
count, an error string, then the hit fields: `vcpu`, `threadId`, `rip`, `rsp`, `cr3`, `address`, `bytesAtRip` (up to
`kDebugBreakpointBytes` 16 bytes), `module`, `moduleOffset`, `exportFound/exportAddress/exportOffset/exportName` and
`pdbFound/pdbSymbol/pdbDisplacement`, then `guestPid`, `guestTid`, `processFound` and `threadFound` with the same
meaning as in `DebugRegisterRecord`, and finally `unverified` (`u8`). `unverified = 1` marks a hardware-breakpoint hit
accepted through the target module map because the stop CR3 matched neither process DTB; the hit may belong to
another process that shares the address. Teardown removes the breakpoint, closes the RSP connection, stops a stub the
host started, then resumes a guest it paused or re-pauses an already-paused one.

A hardware breakpoint uses one of the four debug-register slots, so it competes with an active watchpoint; the host
already refuses concurrent debug operations with `Busy`, which keeps the slot budget simple. A software breakpoint
patches guest memory through the selected vCPU's address space; the host therefore selects a thread in the target
process for a user-mode address, and answers `InvalidState` when none is identifiable. Kernel addresses are always
mapped. The host requires the stop PC to equal the requested address, so an unrelated stop is skipped rather than
reported as a hit. Snapshots, `system_reset` and migration drop both the breakpoint and the stub; a request must not
span one of those operations.

## Debug single step

`DebugStepRequest` carries a `u32` vCPU index (default `0`) followed by a string GDB thread id. A non-empty
`threadId` selects that thread and ignores `vcpu`; the CLI and MCP reject supplying both. The request is valid only
while the guest is paused and no other owner holds the debug channel; otherwise the host answers `InvalidState`. It
starts a stub, selects that thread, executes one instruction with `vCont;s` (`s` when `vCont` is unavailable),
captures the resulting registers and tears the stub down while leaving the guest paused. `DebugStepResponse` is one
fixed `DebugRegisterRecord`, the same layout as one entry of `DebugRegistersResponse`, including the `threadId` of
the thread that actually stepped. A vCPU index or thread id that is not present in the stub's thread list is
reported as `InvalidState` with the reason; use `debug-threads` to discover them.

## Physical memory variants

`PageReadRequest`, `PrefetchRequest` and `MemorySearchRequest` carry a `pid` field. Sending `kPhysicalPid`
(`0xFFFFFFFF`, MemProcFS `dwPID = -1`) in that field addresses the bound target's own physical memory instead of a
process address space; the payload layout is unchanged. Every other pid-scoped request needs a real process id,
including `VirtualQueryRequest`, because `VMMDLL_MemVirtualQuery` has no physical-memory branch.

## Config options

`ConfigSetRequest` exposes only an allow-list of `VMMDLL_ConfigSet` options: the cache/tick tuning options,
the refresh triggers, and the process-DTB forcing options. `option` is the stable `ConfigOption` wire enum; the host
maps it to the vendored `VMMDLL_OPT_*` constant, so no MemProcFS value appears on the wire. `value` is passed
through as `qwValue` and must be present (and nonzero page-aligned) for the process-DTB options; refresh triggers
ignore it. `pid` is required for `refresh-specific-process`, `process-dtb` and `process-dtb-fast-lowintegrity` and
must be zero otherwise; the host ORs it into the low dword of `fOption` as MemProcFS documents.

## VFS writes

`VfsWriteRequest` carries `path`, `offset` and raw `data`; the answer is `bytesWritten`. The capability is
**opt-in**: unless the host was started with `--allow-vfs-write`, every request is answered with
`ErrorResponse(AccessDenied)` and `CapabilitiesResponse` does not list the type. The client splits a larger write
into chunks that each fit in one `kMaxPayloadLength` frame and advances `offset` per chunk.

## Error codes

`ErrorCode` in `frame.hpp`. Names come from `errorCodeName` in `frame.cpp`.

 Code | Name | Sent when |
------|------|-----------|
 1 | `MalformedPayload` | decode failed, or a request field fails validation |
 2 | `UnsupportedVersion` | header version ≠ `kProtocolVersion` |
 3 | `UnknownMessage` | type not handled by `Session::dispatch` |
 4 | `TargetNotFound` | bind name not in the current target scan |
 5 | `TargetUnavailable` | `VmBackend::acquire` failed |
 6 | `SessionAlreadyBound` | second `BindRequest` on a connection |
 7 | `Internal` | provider failed and no more specific code applies |
 8 | `NotBound` | target-scoped request before a successful bind |
 9 | `CapabilityUnavailable` | the bound handle does not implement the provider interface |
 10 | `ResultTooLarge` | single-frame response > 1 MiB, or a request above a count cap (pages, search hits, …) |
 11 | `UnknownScatter` | scatter id not open in this session |
 12 | `InvalidRange` | zero-size, wrapping or over-`kMaxReadSize` range; empty page range; search window with `max <= min` |
 13 | `ScatterLimitReached` | more than `kMaxScatterHandlesPerSession` scatter handles |
 14 | `UnknownVm` | `vmIndex` not in the `vm-list` result (physical read/write/translate/scatter) |
 15 | `NotFound` | provider reported `LookupStatus::NotFound` (missing pid, module, export, VA region, VFS path) |
 16 | `AccessDenied` | peer refused by the access policy, or `VfsWriteRequest` while `--allow-vfs-write` is off |
 17 | `Cancelled` | request cancelled before it sent a frame |
 18 | `Timeout` | request budget expired before it sent a frame |
 19 | `Busy` | worker pool and pending queue are both full |
 20 | `InvalidState` | `DebugRegistersRequest`, `DebugThreadsRequest`, `DebugBacktraceRequest` or `DebugStepRequest` while the guest is running, another stub owns the debug channel, a step/backtrace selects an unknown thread, or a software user-mode breakpoint cannot find a vCPU in the target process |

Codes are additive: adding a code or a message type keeps `kProtocolVersion` at `1`. Do not change the layout of an
existing payload; if a change is unavoidable, ask the user first, because old clients will misparse it.

## Limits

All in `src/core/ipc/frame.hpp` unless noted. Validate against them in the session before calling a provider.

- Transport: `kMaxPayloadLength` 1 MiB, `kFrameHeaderSize` 20.
- Memory: `kMaxReadSize` 64 MiB per read request. `readMemory` / `readPhysicalMemory` in `client_memory.cpp` / `client_system.cpp` split larger
  reads into 64 MiB requests.
- Scatter: `kMaxScatterHandlesPerSession` 8, `kMaxScatterEntries` 4096, `kMaxScatterReadBytes` 64 MiB. Physical
  scatter entries: at most one page, never crossing a page, at least `kPhysicalScatterMinBytes` 8 bytes.
  `ScatterExecuteRead` re-reads the ranges prepared on the handle without re-running writes and reports only the
  read entries. `ScatterClear` resets the prepared entries and the per-handle counters, so the handle can take a
  new batch without being closed.
- Streams: `kMaxMapBatchBytes` 512 KiB per data frame, `kMaxMapStringLength` 1024.
- Per-area entry caps: handles `0x10000`, module tables `0x10000`, unloaded modules `0x1000`, services `0x1000`,
  users `0x400`, network `0x10000`, heaps `0x100`, heap allocations `0x40000`, kernel drivers `0x400`, devices
  `0x1000`, objects `0x8000` (children per object `0x40`), PTE `0x20000`, pool `0x20000`, pool tags `0x20000`,
  PFN `0x8000`.
- Pages: `kMaxPageCount` 4096, `kMaxPrefetchPages` `0x10000`. Search: pattern ≤ 32 bytes, hits ≤ `0x10000`,
  alignment a power of two ≤ 4096. Vad-ex: `pageCount` 1–`kMaxPageCount`. Debug registers: at most
  `kMaxDebugVcpuRecords` 256 records. Disassembly: at most `kMaxDisassembleInstructions` 128 instructions and
  `kMaxDisassembleBytes` 1920 read bytes. Backtrace: at most `kMaxDebugBacktraceFrames` 128 frames and
  `kMaxDebugBacktraceDepthBytes` 1 MiB above the starting `rsp`.
- In `message.hpp`: `kMaxModuleSections` 96, `kMaxCallstackFrames` 64, `kModuleDataDirectoryCount` 16.

A cap that cuts a stream short is reported through the `*End` frame's `truncated` flag, not as an error.

## Cancellation and deadlines

- `CancelRequest` payload: `u64` target request id. Answer: `CancelResponse` = `u8 cancelled` + `u64 requestId`.
- `Session::dispatch` handles `CancelRequest` without touching in-flight state. Every other request restarts the
  deadline (`--request-timeout`, default 30 s, `0` disables), sets `_inFlight`, and clears both on return.
- A cancel that names nothing in flight (or an already-cancelled request) gets `cancelled = 0`.
- A cancel that arrives before the target request sent a frame produces, in this order:
  `ErrorResponse(Cancelled)` for the target id, then `CancelResponse(cancelled = 1)`.
- A cancel that arrives after frames were streamed ends the connection.
- Because requests are serial, a cancel is only seen while a handler polls `Session::requestAborted`, which consumes
  a fully buffered `CancelRequest` with `bufferedFrameHeader`. Call it before every data frame in a stream loop.
- Deadline expiry follows the same split: `Timeout` error before the first frame, connection end after it.
  `Session::sendFrame` turns the first response of an expired request into `ErrorResponse(Timeout)` on its own.

## Connection refusals

`AccessDenied` and `Busy` are sent before any `Session` exists. The server waits for the peer's first frame
(`refuseAfterFirstFrame`, `src/core/server/server.cpp`) so the refusal can echo its request id, then closes.

## Adding a message type (checklist)

Do every step; a missed step compiles but fails at runtime.

1. `frame.hpp`: add the request (next free `0x00xx`) and its response values (next free `0x80xx`). Mark streamed
   ones with the `// streamed:` comment.
2. `frame.cpp`: add every new value to `messageTypeName`.
3. `message.hpp` / `message_<area>.cpp`: add the structs and `encode*`/`decode*` pairs, keeping the `atEnd()` check.
   For a new descriptor, add `write*`/`read*` helpers in `message_codec.hpp` **and** a matching `*EncodedSize` in
   `src/core/server/map_batching.hpp`. These three must agree field for field.
4. `src/core/server/<area>_provider.hpp`: add an abstract provider interface (pure filter helpers go in a
   `<area>_provider.cpp` and into the `mpfshost_core` source list in `CMakeLists.txt`).
5. `session.hpp` / `session_<area>.cpp`: declare the handler and implement it in the family file matching the
   message family; add the `case` to `Session::dispatch` in `session.cpp`.
   Follow the order bound → decode → validate → `dynamic_cast` provider → call → stream or send.
6. `src/host/vmm_<area>.cpp`: implement the provider on `VmmHandle` (add the base class in `vmm_handle.hpp`, the file
   in `mpfshost_host`).
7. `client.hpp` / `client_<area>.cpp`: add a `<Area>Query` and a free function using `connectAndBind` + `exchange` /
   `exchangeStreamed`.
8. `src/cli/`: add the `--client` command — the flags in `options.cpp`, the rendering in `run_client.cpp`, and the
   usage text in `usage.cpp`.
9. Tests: codec round trip (`tests/message_test.cpp` style), session/client round trip through
   `tests/support/fake_backend.hpp` (extend `FakeHandle` with a `Fake<Area>State`), and a `BareHandle` case for
   `CapabilityUnavailable`.
10. Docs: add the row to the table above and update `README.md`'s command table.
