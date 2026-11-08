# mpfshost architecture

Read this before you change threading, the server, the session lifecycle, the handle cache, access control,
anything under `src/host/`, or the test fakes. For the wire format read [`PROTOCOL.md`](PROTOCOL.md).

## Purpose

One privileged process (`mpfshost`, run as root) holds MemProcFS handles to QEMU/KVM guests and serves them to many
unprivileged clients over a Unix socket. Clients never load `vmm.so` and never need root.

## Build targets and layering

 Target | Sources | Links | May use |
--------|---------|-------|---------|
 `mpfshost_core` (static) | `src/core/**` | `Threads::Threads`, `nlohmann_json::nlohmann_json` (private) | standard library, `nlohmann/json` in `src/core/mcp/` and `src/core/debug/` only |
 `mpfshost_host` (static) | `src/host/**` | `vmm.so`, `leechcore.so`, `mpfshost_core` | `vmmdll.h`, root-only paths |
 `mpfshost` (exe) | `src/main.cpp`, `src/cli/**` | `mpfshost_host` | both |
 `mpfshost_tests` (exe) | `tests/**` | `mpfshost_core`, `Catch2::Catch2` | never `mpfshost_host` |

Rules:

- Nothing under `src/core/` includes `vmmdll.h`, `leechcore.h` or anything from `src/host/`.
- Raw MemProcFS numbers (pool type, PFN type, module kind, …) travel on the wire as integers. The CLI
  (`src/cli/render.cpp`) renders their names (`poolTypeName`, `pfnTypeName`, `moduleKindName`, …), so `core` stays
  MemProcFS-free.
- `src/cli/` is part of the executable only. `mpfshost_core` never sees it, and `src/core/` never includes it.
- `mpfshost_core` is also the installed client library (`libmpfshost_core.a` plus `client.hpp`, `message.hpp`,
  `frame.hpp`, `version.hpp`). Treat those four headers as public API.
- `nlohmann/json` is a private, header-only dependency of `mpfshost_core`, used only by `src/core/mcp/`. It must not
  appear in an installed public header; the MCP headers expose plain strings and structs.

## Runtime object graph

`runServer` in `src/main.cpp` builds the host:

```
FilesystemTargetSource targets;      // src/host/filesystem_target_source.*
VmmHandleFactory       factory;      // src/host/vmm_handle_factory.*  (VmBackend)
CachingBackend         backend{factory, ...};   // src/core/server/handle_cache.*  (VmBackend)
Server                 server{options, targets, backend};   // src/core/server/server.*
```

**The declaration order is load-bearing.** Destruction runs in reverse order: `Server` joins every worker (and so
every `Session`) first, then the cache drops its handles, and last `~VmmHandleFactory` calls `VMMDLL_CloseAll()`.
Do not reorder these lines or move ownership elsewhere.

## Server and threads

`Server::run` (`src/core/server/server.cpp`):

1. Creates the socket (mode `--socket-mode`, group `--socket-group`), `listen(SOMAXCONN)`.
2. Starts `--workers` threads (default 8) running `workerLoop`.
3. The accept thread loops on `accept4`. For each client it:
   - applies `SO_RCVTIMEO` (`--idle-timeout`, default 300 s) and `SO_SNDTIMEO` (the request budget),
   - reads `SO_PEERCRED` and evaluates the `AccessPolicy` once,
   - pushes a `PendingConnection {fd, connectionId, credentials, decision}` into a queue capped at
     `--pending-connections` (default 64).
4. If the queue is full, the accept thread hands the client to a separate, bounded refusal queue (16 slots) served
   by one dedicated thread. That thread answers the client's first frame with `ErrorResponse(Busy)` (waiting up to
   1 s) and closes the socket, so the accept loop never blocks on a slow or silent peer. When the refusal queue is
   also full, the accept thread closes the client immediately without a response.
5. A worker pops a connection. A denied peer gets `ErrorResponse(AccessDenied)` (`rejectPeer`) and is closed.
   An allowed one runs `Session {fd, ...}.run()` on the worker's stack.

`requestStop()` (from `SIGINT`/`SIGTERM` in `src/main.cpp`) stops the accept loop and the pool; `~Server` joins the
workers and drains the queue.

## Session lifecycle

`Session` (`src/core/server/session.{hpp,cpp}`) owns exactly one connection.

- **Ownership:** the session owns the fd and closes it in `~Session`. It also owns its scatter handles (`_scatters`)
  and its `BoundTarget` (`shared_ptr<VmHandle>`). Nothing else may close the fd.
- **Loop:** `run()` reads one frame, calls `dispatch()`, and repeats. It ends on `Closed`, on `Timeout` (idle), after
  answering a `Rejected` frame, or when `_writeFailed` is set.
- **Per-request state:** `dispatch()` restarts `_deadline`, sets `_inFlight = requestId`, resets `_framesForRequest`
  and `_cancelled`, and a scope guard clears them on return. `CancelRequest` bypasses this so it does not clobber
  the request it targets.
- **Binding:** `handleBind` runs once per session. It rescans `TargetSource`, matches the name, calls
  `VmBackend::acquire`, and stores `_bound`. Every target-scoped handler starts with `if (!_bound) → NotBound`.
- **Capabilities:** a handler gets its capability with `dynamic_cast<XxxProvider*>(_bound->handle.get())`. A null
  result → `CapabilityUnavailable`. `VmHandle` itself is empty; all behaviour lives in the provider interfaces in
  `src/core/server/*_provider.hpp`.
- **Errors from providers:** providers return `std::optional<T>` or `bool` with a `std::string& error` and, for
  lookups, a `LookupStatus& status` (`Ok`, `NotFound`, `Failed`). Map `NotFound` to `ErrorCode::NotFound`, everything
  else to `Internal` unless a more specific code exists.
- **Streaming:** handlers get the full result from the provider first, then send it in batches with
  `batchCount(entries, offset, kMaxMapBatchBytes, <entry>EncodedSize)` from `map_batching.hpp`. Before each batch
  they call `requestAborted(requestId, code)` and check `_writeFailed`. On abort they call
  `abortRequest(requestId, code)` and return.
- **Sending:** use only `sendError`, `sendPayload` (single frame, checks the 1 MiB limit) and `sendFrame`. They enforce
  the framing rules in [`PROTOCOL.md`](PROTOCOL.md).

## Handle cache and sharing

`CachingBackend` (`src/core/server/handle_cache.{hpp,cpp}`) wraps the factory:

- It keeps one strong `shared_ptr<VmHandle>` per target name, guarded by a mutex.
- A cold `acquire` runs outside the lock with per-target single flight, so two sessions binding the same VM create
  one handle.
- A target whose `qemuPid` or `qmpSocket` changed gets a fresh handle. Failed acquisitions are not cached.
- An idle handle is dropped after `--handle-idle-timeout` (default 60 s; `0` keeps it until shutdown). A sweeper
  `std::jthread` runs every idle/4, clamped to 1–30 s (`handleCacheOptions` in `src/main.cpp`). Sessions keep their
  own reference, so eviction never closes a handle under a running session.

`VmmHandleFactory::acquire` creates a new `VMM_HANDLE` on every call (`VMMDLL_Initialize`, then
`VMMDLL_InitializePlugins`, then a 500 ms settle sleep) and tracks it with a `weak_ptr` for the shutdown report.

**Threading consequence:** several sessions on different worker threads can call the same `VmmHandle` at the same
time. `VmmHandle` has no mutex and relies on MemProcFS being thread-safe per handle. Do not add per-session mutable
state to `VmmHandle`; keep per-request state on the stack or in `Session`.

## Debug control

`src/core/debug/qmp_client.{hpp,cpp}` speaks QEMU's QMP control channel over the target's Unix socket. It owns the
JSON line framing, the greeting and `qmp_capabilities` handshake, command/reply matching, skipping of asynchronous
events, and bounded waits. `QmpTransport` is injectable, so tests script replies without a VM.

The host side is `src/host/vmm_debug.{hpp,cpp}`: `VmmHandle` owns one `DebugController`, constructed from
`VmTarget.qmpSocket`, and the controller serialises requests with its own mutex because sessions share the handle.
No per-session debug state goes on `VmmHandle`. Each request opens a fresh QMP connection, so a lost socket does not
poison the handle. Stop/continue set `hostPaused` only when this host performed the pause; `stubActive` and
`watchCount` are reserved for later debug slices.

QMP socket access is root-equivalent over the guest. There is no extra host flag; the connection's access policy is
the only gate, and stop/continue are logged.

## Request deadlines

`src/core/server/request_deadline.{hpp,cpp}`:

- `RequestDeadline` holds the budget (`--request-timeout`, default 30 s, `0` disables).
- `RequestDeadlineScope` publishes it thread-locally for the duration of `Session::dispatch`.
- Host code polls the free function `requestExpired()` at safe points (chunk boundaries, map copy loops, the
  `VMMDLL_MemSearch` callback via `fAbortRequested`). A single MemProcFS call cannot be interrupted.
- When a host backend returns early because the deadline expired, the session's next send turns into
  `ErrorResponse(Timeout)` (before any frame) or a connection end (after frames).

## Access control

- `AccessPolicy` (`src/core/server/access_policy.{hpp,cpp}`) is an allow-list of uids and gids. Empty = open.
- `main()` adds group `mpfshost` unless `--allow-all` is given; `--allow-uid/--allow-user/--allow-gid/--allow-group`
  add entries. If the group cannot be resolved, the host refuses to start. `--allow-all` combined with an
  `--allow-*` flag is a startup error.
- uid 0 is always allowed.
- Membership is matched against the peer's primary gid and the supplementary groups from `/proc/<pid>/status`
  (`readPeerCredentials`), never NSS.
- Socket mode is `0660` and owned by the group when enforcing, `0666` when open.
- `src/core/client/connect_diagnostics.cpp` explains a kernel-denied `connect()` on the client side.

## Targets

`FilesystemTargetSource::scan()` pairs `/run/libvirt/qemu/<vm>.pid` (QEMU pid) with `/var/qmp-sockets/<vm>.sock`. A
VM is a target only when both exist. `src/core/target/scanner.*` holds the pairing logic over injectable directories
(tested in `tests/scanner_test.cpp`). The device string is
`qemu://hugepage-pid=<pid>,qmp=<socket>` (`src/host/device_string.cpp`).

## Host backends (`src/host/`)

`VmmHandle` (`vmm_handle.hpp`) inherits every provider interface. Its methods are implemented in one file per area:
`vmm_memory`, `vmm_scatter`, `vmm_physical`, `vmm_vm`, `vmm_processes`, `vmm_modules`, `vmm_module_tables`,
`vmm_unloaded_modules`, `vmm_maps`, `vmm_threads`, `vmm_handles`, `vmm_services`, `vmm_users`, `vmm_network`,
`vmm_heaps`, `vmm_kernel_drivers`, `vmm_kernel_devices`, `vmm_kernel_objects`, `vmm_pte`, `vmm_pool`, `vmm_pfn`,
`vmm_vfs`, `vmm_search`, `vmm_debug`. Shared helpers: `vmm_text.hpp` (`copyMapText`, `formatStatus`), `vmm_process_probe.hpp`
(`processPresent`).

MemProcFS rules the backends follow (copy them in new code):

- Every pointer returned by a `VMMDLL_Map_*` call is owned by MemProcFS. Release it with `VMMDLL_MemFree` through an
  RAII guard, including on error paths. Copy every string out (`copyMapText`, capped at `kMaxMapStringLength`)
  before the guard runs.
- `VMMDLL_Map_GetPfn` is the exception: it fills a caller buffer (an 8-byte-aligned `std::vector<std::uint64_t>`),
  so nothing is freed with `VMMDLL_MemFree`.
- Before `VMMDLL_ProcessGetInformation`, zero the struct, set `magic`/`wVersion` and pass `size =
  sizeof(VMMDLL_PROCESS_INFORMATION)`.
- Use `dwPID = (DWORD)-1` for the target's own physical memory; child partitions use the `VMMDLL_VmMem*` group.
- Large reads loop in chunks; a short read is a failure.
- Per-pid maps probe a failed call with `processPresent()` and answer `NotFound` for a vanished pid. Whole-machine
  maps (services, users, net, kernel maps, pool, pfn) have no pid to probe and answer `Internal`.

MemProcFS behaviours observed on a live Windows 11 24H2 guest, which the code depends on:

- `VMMDLL_MemSearch` returns failure when the result callback returns `FALSE`, so a hit-capped or aborted search is
  reported as success with the hits so far. A term whose pattern bytes are all zero always fails, so the session
  rejects it as `MalformedPayload`. Hits outside `[min, max)` are dropped by the host.
- `VMMDLL_ProcessGetSectionsU` fills the buffer only when `cSections` equals the exact section count; query the count
  first and reject counts above `kMaxModuleSections`.
- `VMMDLL_VfsListU` returns success with zero entries for a missing path. `VmmHandle::listVfs` lists the parent to
  tell "missing" (`NotFound`) from "empty"; this works for nested paths only, not top-level names.
- `VMMDLL_MAP_NETENTRY.uszText` is a pre-rendered row, not a process name; the host resolves the process per pid and
  appends the port to the address. `dwState` is MemProcFS's own 0-based value.
- `VMMDLL_Map_GetPool` builds the whole map and cannot be interrupted; `vmm_pool.cpp` checks `requestExpired()`
  before the call and while copying entries.
- Kernel device `dwDeviceType` is `0` while `uszDeviceType` is set; built-in drivers report `cbDriverSize = 0`.

## Logging

`src/core/log/` (`mpfshost::log::error/warn/info/debug`) writes one line per record to `stderr`: ISO-8601 UTC
timestamp, level, `category: message`. Level from `--log-level` or `MPFSHOST_LOG_LEVEL` (flag wins). Tests capture
output with `log::setSink()`. Never log guest memory contents.

## Client side

- `src/core/client/client.hpp` plus `client_<area>.cpp`: one free function per capability, each taking
  `(socketPath, <Area>Query, error)` and returning `std::optional<T>`. The split mirrors the server families:
  `client_{memory,process,maps,system,kernel,vfs,debug}.cpp`. `client_transport.{hpp,cpp}` (internal, not installed) holds
  the shared connection helpers. Each call opens its own connection (`connectToServer`), binds if needed
  (`connectAndBind`), and uses `exchange` or `exchangeStreamed`.
- `ScatterSession` is an RAII wrapper over one connection plus one scatter id (create → prepare → execute → close).
- `src/core/client/cli_options.{hpp,cpp}` owns `ClientOptions` and `parseClientOptions`, which turns the
  `--client` half of a command line into that struct and reports every flag and required-option error. It lives in
  `core` so `tests/cli_options_test.cpp` can exercise it without linking `src/cli/`.
- `src/cli/run_client.cpp` `runClient` maps `--client <command>` to these functions and renders output with
  `printf`. `src/cli/options.cpp` parses only the server flags and hands the remaining tokens to
  `parseClientOptions`.

## MCP mode

`mpfshost --mcp` is a second, unprivileged front end in the same binary. It speaks the Model Context Protocol as
JSON-RPC 2.0, one object per line, over stdin/stdout. It is a **client of the root host**, not a host: every tool
call goes through `src/core/client/client.hpp` to the Unix socket, so the host's access policy, request deadlines and
MemProcFS ownership rules still apply. MCP mode never loads `vmm.so` and never needs root.

- `src/core/mcp/mcp_server.{hpp,cpp}` owns the stream state machine: JSON parsing, protocol-version negotiation
  (2025-06-18, 2025-03-26, 2024-11-05), the initialize/notification ordering, `ping`, `tools/list`, `tools/call`,
  JSON-RPC errors and `isError` tool results. It takes `std::istream&` and `std::ostream&` so tests drive it
  in-process. stdout carries only JSON-RPC; logs go through `mpfshost::log` to stderr.
- `src/core/mcp/mcp_tools.{hpp,cpp}` holds the tool catalogue and the argument/result mapping. One tool exists per
  read capability in `client.hpp`; the write capabilities (`config_set`, `write_memory`, `write_vfs`,
  `write_physical_memory`, `debug_stop`, `debug_continue`) are exposed and annotated destructive. The host's
  `--allow-vfs-write` check is still the final authority for `write_vfs`.
- Argument names and limits follow the CLI flags and `docs/PROTOCOL.md`. Results convert 64-bit values to hex
  strings, render bytes as hex plus ASCII, and apply `limit`/`offset`/`truncated` to list tools. Guest-derived data
  is never logged.
- `src/cli/options.cpp` only recognises `--mcp` and rejects it alongside `--client` or server-only flags; `main()`
  dispatches to the MCP server before the root check and access-group lookup. The implementation stays out of
  `src/host/` and `src/cli/`, and the wire protocol is unchanged.

## Testing architecture

- `tests/support/fake_backend.hpp` (namespace `mpfshost::testing`):
  - `FakeTargetSource` — `setTargets(...)`.
  - `FakeBackend` — a `VmBackend` whose `FakeHandle` implements most provider interfaces. Configure it with
    `set<Thing>(...)` and `fail<Thing>With(message[, LookupStatus])`; inspect the `Fake<Area>State` captured
    `lastRequest` through `<area>State()`. `failWith(...)` makes `acquire` fail.
  - `BareHandle` — implements no provider; use it to test `CapabilityUnavailable`.
  - `CountingBackend` — counts and blocks `acquire`; used by the handle-cache tests.
- `FakeHandle` does **not** implement `PteProvider`, `PoolProvider` or `PfnProvider`; those session paths have no test
  yet (see `TODO.md`). It implements `DebugProvider` through `FakeDebugState`.
- Server and client tests start a real `Server` on a socket under `MPFSHOST_TEST_TMP_DIR` (`./tmp`) and call the
  real client functions against it. No test touches MemProcFS, `/run` or `/var`.
- `tests/mcp_protocol_test.cpp` drives `mcp::McpServer` over in-memory streams for version negotiation, ordering,
  JSON-RPC errors, EOF and tool-list gating. `tests/mcp_tools_test.cpp` runs the same server against a real `Server`
  with `FakeBackend` for representative tools, host errors, an unreachable socket, `CapabilityUnavailable` and the
  write-tool switch. The tool catalogue test asserts that every read capability in `client.hpp` has a tool.
- `tests/main.cpp` refuses to run as root.
- Live behaviour (anything in `src/host/`) is verified only by the user against an installed host.

## Install layout

`cmake --install` puts `mpfshost`, `info.db`, `MemProcFS/*.so` and empty `Symbols/` and `plugins/` in `bindir`
(MemProcFS resolves them relative to the executable; the binary has `RUNPATH=$ORIGIN`). It also installs the client
headers, `libmpfshost_core.a`, `mpfshost.pc`, and `mpfshost.service` under `libdir/systemd/system`. `README.md`, the
`docs/*.md` files and the client examples (README, `CMakeLists.txt` and sources) go under `docdir`, with the examples
in `docdir/examples`. The build tree mirrors the runtime part through a post-build step.
