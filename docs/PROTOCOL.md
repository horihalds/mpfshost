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

Requests are `0x0001`–`0x0038`; responses are `0x8001`–`0x805B` plus `ErrorResponse` = `0x8FFF`. Values
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
  alignment a power of two ≤ 4096. Vad-ex: `pageCount` 1–`kMaxPageCount`.
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
