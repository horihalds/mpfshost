# AGENTS.md

Rules for any agent working in this repository. Read all of it before you change anything.

`mpfshost` is a root-run host that holds MemProcFS handles to QEMU/KVM guests and serves them to unprivileged
clients over a Unix socket with a length-prefixed binary protocol. One binary is both the host and the `--client` CLI.

## Read more only when needed

- Read `docs/PROTOCOL.md` before you add or change a message type, an `encode*`/`decode*` function, a
  `Session::handle*` method, a client call in `src/core/client/client_*.cpp`, or anything that sends frames.
- Read `docs/ARCHITECTURE.md` before you touch threading, `Server`, `Session` lifecycle, `CachingBackend`, access
  control, deadlines, anything in `src/host/`, or `tests/support/fake_backend.hpp`.
- Read `TODO.md` when you are asked to pick the next task.
- `README.md` is for humans: install, run, CLI usage.

## Hard rules

- Never run `sudo`. If something needs root, give the user the exact command and wait for their output.
- Never start the host (`build/mpfshost` without `--client`). Never run `scripts/install.sh`. The user does both.
- Never add a dependency, install a package, download anything or access the internet without explicit permission.
- Never modify `MemProcFS/` (vendored; changed only by `scripts/fetch-memprocfs.sh`, run on request) or
  `reference/` (read-only example).
- Never read `MemProcFS/` beyond `vmmdll.h` declarations you actually need.
- Put every scratch file in `./tmp/` (gitignored). Create nothing elsewhere.
- Use C++23 only. Use Ninja only. Do not add other build systems.
- Build with `-Wall -Wextra -Werror`; there are no warning suppressions. Fix every warning in our source. The vendored
  `MemProcFS/` headers are marked `SYSTEM` so their own warnings are not attributed to us. Never add a new `-Wno-*`,
  `#pragma` or cast just to silence a warning; ask first if a suppression seems unavoidable.
- Never run tests or the test binary as root. `tests/main.cpp` refuses to, by design; do not remove that guard.
- Commit when the task is complete (see "Done checklist"). Never leave the tree dirty.

## Git commits

Published commits are authored as `horihalds`; never expose a local account or agent identity.

- Identity: commit as `horihalds`. The machine-wide `user.name` and `user.email` are empty, so the identity is set
  per repository. Check it with `git var GIT_AUTHOR_IDENT`; if it fails, set `user.name` to `horihalds` and
  `user.email` to the account's GitHub noreply address with `git config`, then commit. Never commit as a generic
  agent name or any other local account.
- Message: one imperative, sentence-case summary line with no `type(scope):` prefix and no trailing period. Keep it
  under ~72 columns. Add a body wrapped at ~100 columns when the reason is not obvious. Match the existing style:
  `Add opt-in VFS write capability`, `Fix small CLI output placeholders and heap selector text`.
- Scope: stage only the paths for the change (`git add <paths>`). Never use `git add -A` or `git commit -a` blindly;
  keep unrelated formatting and documentation churn out of the commit.
- One logical change per commit. Do not bundle a feature, a refactor and a documentation rewrite.
- No attribution trailers (`Co-authored-by`, `Generated-by`) and no agent or tool credit in the message.
- Do not amend, rebase or force-push published commits unless the user explicitly asks. The remote is
  `origin git@github.com:horihalds/mpfshost.git`; push as `horihalds`:
  ```sh
  GIT_SSH_COMMAND='ssh -i ~/.ssh/id_ed25519_horihalds -o IdentitiesOnly=yes' git push origin master
  ```
- Commit only after the "Done checklist" passes; never leave the tree dirty.

## Commands (all verified)

```sh
cmake -S . -B build -G Ninja                  # configure once (Catch2 2.13 must be installed; first run fetches nlohmann/json)
ninja -C build                                # build everything; must print no warnings
ctest --test-dir build --output-on-failure    # full suite, ~8 s
ctest --test-dir build --output-on-failure -j8   # full suite, ~1-2 s
ctest --test-dir build -R 'scatter'           # tests whose name matches a regex
build/tests/mpfshost_tests '[pool]'           # one Catch2 tag
build/tests/mpfshost_tests 'exact test case name'
build/tests/mpfshost_tests --list-tags        # all tags
clang-format --dry-run --Werror <files>       # format check; non-zero exit = not formatted
clang-format -i <files>                       # format in place
./build/mpfshost --version                    # sanity check that the binary links
```

- If `build/` does not exist, run the configure command first. Never configure or build as root:
  `catch_discover_tests` runs the test binary during the build, and it refuses root.
- `-DBUILD_TESTING=OFF` skips the tests and the Catch2 lookup. Do not use it when verifying a change.
- `scripts/configure.sh` and `scripts/build.sh` wrap the commands above. Prefer the raw commands.

## What can be tested without a VM

- Everything in `mpfshost_core`: frame/message codecs, target scanning, `Session` dispatch, `Server`, the client
  library, the handle cache, access policy, deadlines, cancellation, filters, batching, hex and page-range helpers.
- Tests run a real `Server` on a socket under `./tmp` against `tests/support/fake_backend.hpp`. Configure behaviour
  with `FakeBackend` setters (`set<Thing>`, `fail<Thing>With`); use `BareHandle` for `CapabilityUnavailable`.
- Nothing in `src/host/` is unit-tested. It needs MemProcFS, root and a live VM. If you change `src/host/`, build it,
  then tell the user to run `scripts/install.sh --skip-build` and wait for them to confirm the host is ready before
  you run any `./build/mpfshost --client ...` command.
- `tests/CMakeLists.txt` must never link `mpfshost_host`. `ldd build/tests/mpfshost_tests` must show no `vmm.so`
  and no `leechcore.so`.

## Done checklist

Do all of these before you finish, in order:

1. `ninja -C build` succeeds with no warnings.
2. `ctest --test-dir build --output-on-failure -j8` passes completely.
3. `clang-format -i` on every C++ file you changed, then `clang-format --dry-run --Werror` on them passes.
4. Update docs touched by the change (`docs/PROTOCOL.md` message table, `README.md` command table, `TODO.md`).
5. `git status` shows only your intended changes; then commit as `horihalds` following "Git commits".

## Directory layout

 Path | Contents |
------|----------|
 `src/main.cpp` | Entry point: `main`, signal handling, `runServer` (the host bootstrap). |
 `src/cli/` | The client side of the same binary: `options.*` (`parseCommandLine` over the server flags), `run_client.*`, `render.*` (raw wire enums to text), `usage.*` (`printUsage`). Never linked into `mpfshost_core`. |
 `src/core/ipc/` | `frame.{hpp,cpp}`: header, `MessageType`, `ErrorCode`, limits, `ByteWriter`/`ByteReader`, `readFrame`/`writeFrame`. `message.hpp` + `message_<area>.cpp`: payload structs and codecs; `message_codec.hpp` holds the internal descriptor helpers. |
 `src/core/server/` | `session.*` per-connection dispatcher, `server.*` socket + worker pool, `handle_cache.*`, `access_policy.*`, `request_deadline.*`, `map_batching.hpp`, `backend.hpp`, one `*_provider.hpp` interface per capability. |
 `src/core/client/` | `client.*` one function per capability, `cli_options.*` (`ClientOptions`, `parseClientOptions`), `connect_diagnostics.*`. |
 `src/core/mcp/` | MCP stdio front end: `mcp_server.*` (JSON-RPC state machine) and `mcp_tools.*` (tool catalogue and mapping over `client.hpp`). |
 `src/core/target/` | QEMU target discovery logic (`<vm>.pid` + `<vm>.sock` pairing). |
 `src/core/log/` | Levelled thread-safe logger, `mpfshost::log`. |
 `src/core/util/` | Hex parsing/dumps, decimal/range-size parsing (`number.*`), page-range expansion, substring matcher, identity lookups. |
 `src/host/` | Privileged MemProcFS code: `vmm_handle.hpp` (implements every provider), one `vmm_<area>.cpp` per capability, `vmm_handle_factory.*`, `filesystem_target_source.*`, `device_string.*`. |
 `tests/` | Catch2 v2 tests, mostly one `<area>_test.cpp` per area (name new files that way); `tests/support/fake_backend.hpp` fakes. |
 `tools/guest-probe/` | Optional Windows probe cross-compiled with mingw; never linked into the host. |
 `packaging/`, `scripts/` | systemd unit and pkg-config templates; configure/build/install/fetch scripts. |
 `docs/` | `ARCHITECTURE.md`, `PROTOCOL.md`. |

`src/core/` must never include `vmmdll.h` or anything from `src/host/`.

## Where things live in the largest files

Line numbers are approximate and drift. Search for the function name.

- `src/core/ipc/message_codec.hpp` (internal, not installed): `write<Descriptor>`/`read<Descriptor>` entry codecs and
  `readCappedString` in namespace `mpfshost::detail`. The `encode*`/`decode*` pairs live in one
  `message_<area>.cpp` per family (`core`, `memory`, `process`, `maps`, `system`, `kernel`, `vfs`), each including the
  codec header. Add a new pair to the family file, not to the header.
- `src/main.cpp` (~240 lines): includes, signal handling (`g_server`/`g_signal`/`handleSignal`, `signalName`),
  `socketModeText`, `handleCacheOptions`, `runServer` ~85, `main` ~160 (log level, `parseCommandLine`, then either
  `cli::runClient` or the server setup).
- `src/cli/run_client.cpp` (~1460 lines): `runClient`, one `if (options.command == "...")` block per command.
- `src/cli/options.cpp` (~760 lines): anonymous-namespace helpers (`parseU32`, `parseRangeSize`, `parseHeapSelector`,
  `parseOctalMode`, `resolveUserId`), then `resolveGroupId` and `parseCommandLine` (the single argument loop).
- `src/core/server/session.cpp` (under 700 lines): constructor/destructor, `run`, `dispatch` (the `switch` over every
  `MessageType`), bind/list-targets, `requestAborted`, `abortRequest`, `handleCancel`, `sendError`, `sendPayload` and
  `sendFrame`. The `handle<Name>` methods live in the family file that matches the message family:
  `session_{memory,process,maps,system,kernel,vfs,scatter}.cpp`.
- `src/core/client/client_transport.{hpp,cpp}` (internal, not installed): the connection helpers `connectToServer`,
  `connectAndBind`, `exchange`, `exchangeStreamed`, `expectResponse`, `request` and the `FileDescriptor` guard.
- `src/core/client/client_{memory,process,maps,system,kernel,vfs}.cpp`: one public function per capability, split like
  the server families. `readMemory` and `readPhysicalMemory` split reads into `kMaxReadSize` chunks; `ScatterSession`
  lives in `client_memory.cpp`, the physical and VM functions in `client_system.cpp`.

## Invariants that are easy to break

Protocol:

- A single-frame request gets exactly one response frame or one `ErrorResponse`. A streamed request gets zero or
  more `*Data` frames and exactly one `*End`, or one `ErrorResponse` instead of all of them.
- Never send `ErrorResponse` after a data frame of the same request. Send through `sendError`, `sendPayload` and
  `sendFrame` only; never call `writeFrame` from a handler.
- Every frame echoes the request id. Every `decode*` must end with `if (!reader.ok() || !reader.atEnd()) return
  std::nullopt;`.
- `write<X>`, `read<X>` (in `message_codec.hpp`) and `<x>EncodedSize` (in `map_batching.hpp`) must agree field for field.
  If they drift, batches exceed 1 MiB or decode fails.
- Never renumber or reuse a `MessageType` or `ErrorCode` value. Append new values. Never change an existing payload
  layout without asking; `kProtocolVersion` stays `1`.
- Add every new `MessageType` to `messageTypeName` in `frame.cpp` and to the `switch` in `Session::dispatch`.

Session lifecycle:

- One request at a time per connection. `dispatch` resets per-request state; `CancelRequest` must not.
- Target-scoped handlers check `_bound` first (`NotBound`), then decode (`MalformedPayload`), then validate limits,
  then `dynamic_cast` the provider (`CapabilityUnavailable`), then call it.
- In stream loops call `requestAborted(requestId, code)` before every data frame and stop on `_writeFailed`.
- Map `LookupStatus::NotFound` to `ErrorCode::NotFound`, not `Internal`.

Ownership and threading:

- `Session` owns its fd and closes it. `Server` owns worker threads; sessions run on worker stacks.
- `VmmHandle` instances are shared by sessions on different threads through `CachingBackend`. Do not add mutable
  per-session state to `VmmHandle` or to any provider implementation.
- Keep the declaration order in `runServer` (`targets`, `factory`, `backend`, `server`). It guarantees sessions end
  before handles close and `VMMDLL_CloseAll()` runs last.
- Every pointer from `VMMDLL_Map_*` is freed with `VMMDLL_MemFree` through an RAII guard, after strings are copied
  with `copyMapText`.
- Host code reads the request budget only through `requestExpired()`. Do not pass deadlines as parameters.

## Adding a capability

Follow the 10-step checklist in `docs/PROTOCOL.md` ("Adding a message type"). Copy the closest existing capability
(for a streamed whole-machine map, copy `services`; for a per-pid map, copy `handles`) rather than inventing a new
pattern.

## Code conventions

- `.clang-format` is authoritative: 4 spaces, 120 columns, Allman braces, mandatory braces, `char* p`.
- Headers use both `#pragma once` and an include guard `MPFSHOST_<PATH>_HPP`.
- Everything is in `namespace mpfshost`; tests helpers in `mpfshost::testing`. Close with `} // namespace mpfshost`.
- Types `PascalCase`; functions and locals `lowerCamelCase`; private members `_name`; constants `kName`.
- Mark accessors and query functions `[[nodiscard]]`.
- Fallible functions return `std::optional<T>` or `bool` and report text through a `std::string& error` parameter.
  Follow that pattern in new code.
- Use RAII and `std::` containers; no raw `new`/`delete`.
- Comment only why, never what. No commented-out code.
- Tests: Catch2 v2 (`#include <catch2/catch.hpp>`), `TEST_CASE("description", "[tag]")`. Add a new test file to the
  source list in `tests/CMakeLists.txt`. Add a new `src/core` or `src/host` file to its library in `CMakeLists.txt`.

## Do not touch without asking

- `MemProcFS/`, `reference/`, `.clang-format`, compiler flags in `CMakeLists.txt`, `tests/main.cpp` root guard.
- Install rules, `packaging/` and `scripts/` (they are run as root by the user).
- The public client headers `client.hpp`, `message.hpp`, `frame.hpp`, `version.hpp`: change them additively only.
