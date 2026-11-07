# mpfshost

`mpfshost` is a host process for [MemProcFS](https://github.com/ufrisk/MemProcFS) access to QEMU/KVM virtual
machines. Reading a guest's memory through its QEMU process requires root, so the host runs as root, holds the
MemProcFS handles and serves the VM's memory and inspection capabilities to ordinary, non-privileged client programs
over a Unix domain socket. In short: **one privileged server, many unprivileged clients.**

The same binary contains the host and a command-line client (`--client`). An installed tree also ships a static
client library for your own programs.

- Wire format: [`docs/PROTOCOL.md`](docs/PROTOCOL.md)
- Internals: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
- Contributor and agent rules: [`AGENTS.md`](AGENTS.md)
- Open work: [`TODO.md`](TODO.md)

### Requirements

- Linux with libvirt-managed QEMU/KVM guests, with the PID file and QMP socket in the locations described in
  [How targets are discovered](#how-targets-are-discovered).
- Root **only** for the host process. Clients never need root.
- A C++23 compiler, CMake ≥ 3.25 and Ninja.
- `catch2-devel` (Catch2 2.13.x) for the test suite.
- Optional: `x86_64-w64-mingw32-g++` to cross-compile the Windows guest probe.

The MemProcFS libraries, headers and `info.db` are vendored in `MemProcFS/`.

### Build and test

```sh
cmake -S . -B build -G Ninja
ninja -C build
ctest --test-dir build --output-on-failure
```

The build produces `build/mpfshost` and copies `MemProcFS/info.db` and `MemProcFS/*.so` next to it. MemProcFS loads
them from the executable's directory, so run the binary from `build/` or from an install tree. Do not move it on its
own.

The tests never need root and never touch a real VM. The test binary refuses to run as root, and a build tree created
as root breaks test discovery, so build as your normal user. Configure with `-DBUILD_TESTING=OFF` to skip the tests.

### Installing

```sh
cmake -S . -B build -G Ninja           # add -DCMAKE_INSTALL_PREFIX=<prefix> for a non-default prefix
ninja -C build
sudo cmake --install build
```

 Path | Contents |
------|----------|
 `<prefix>/bin/mpfshost` | Host and client binary (`RUNPATH=$ORIGIN`). |
 `<prefix>/bin/*.so`, `<prefix>/bin/info.db` | MemProcFS runtime files, next to the executable. |
 `<prefix>/bin/Symbols/`, `<prefix>/bin/plugins/` | Empty directories MemProcFS expects: PDB cache and plugin path. |
 `<prefix>/include/mpfshost/core/...` | Client headers (`client.hpp`, `message.hpp`, `frame.hpp`, `version.hpp`). |
 `<prefix>/lib*/libmpfshost_core.a` | Client static library. |
 `<prefix>/lib*/pkgconfig/mpfshost.pc` | pkg-config file. |
 `<prefix>/lib*/systemd/system/mpfshost.service` | systemd unit. |
 `<prefix>/share/doc/mpfshost/README.md` | This document. |
 `<prefix>/share/doc/mpfshost/*.md` | `PROTOCOL.md` and `ARCHITECTURE.md`. |
 `<prefix>/share/doc/mpfshost/examples/` | Client example sources and their README. |

The unit and the `.pc` file contain the configure-time prefix, so set `CMAKE_INSTALL_PREFIX` when configuring rather
than passing `--prefix` to `cmake --install`.

#### Scripts

```sh
scripts/configure.sh [--build-dir <dir>] [--prefix <dir>] [--build-type <type>] [--no-tests] [--fresh] [-v]
scripts/build.sh     [--build-dir <dir>] [--target <name>] [--jobs <n>] [--prefix <dir>] [-v]
scripts/install.sh   [--build-dir <dir>] [--prefix <dir>] [--skip-build] [--no-service] [--add-user] [--dry-run] [-v]
```

`install.sh` builds (unless `--skip-build`) as your user, then uses `sudo` only for `cmake --install`, creating the
`mpfshost` group, the optional `usermod -aG` (`--add-user`) and the `systemctl` reload/enable/restart
(`--no-service` skips these). Afterwards it checks `--version`, `RUNPATH` and `ldd`, and runs
`--client list-targets` as you to confirm the socket is reachable. `--dry-run` only prints the privileged commands.

#### Updating the vendored MemProcFS

```sh
scripts/fetch-memprocfs.sh [--version <tag>] [--arch linux_x64|linux_aarch64] [--dest <dir>] [--dry-run] [--list] [--keep-extra]
```

The script downloads the matching upstream Linux release into `tmp/fetch-memprocfs`, installs only the files in its
manifest, records the tag and checksum in `MemProcFS/VERSION` and removes everything else. If you need another
upstream file, add it to the manifest. Do not copy it in by hand.

### Running the host

```sh
sudo ./build/mpfshost                    # development tree
sudo systemctl enable --now mpfshost     # installed tree
journalctl -u mpfshost -f
```

The host listens on `/run/mpfshost.sock` (`--socket <path>` to change it) and stops cleanly on `SIGINT`/`SIGTERM`.
The systemd unit runs it as root with `NoNewPrivileges=yes` but no `Protect*` sandboxing, because MemProcFS needs
broad `/proc` access. Add access options to its `ExecStart` to change the policy.

 Option | Default | Meaning |
--------|---------|---------|
 `--socket <path>` | `/run/mpfshost.sock` | Socket to listen on. |
 `--log-level <level>` | `info` | `error`, `warn`, `info` or `debug`; also `MPFSHOST_LOG_LEVEL` (the flag wins). |
 `--handle-idle-timeout <s>` | `60` | Keep a target's MemProcFS handle this long after its last client; `0` = until exit. |
 `--workers <n>` | `8` | Connections served concurrently. |
 `--pending-connections <n>` | `64` | Accepted connections waiting for a worker; beyond that a client gets `Busy`. |
 `--request-timeout <s>` | `30` | Budget for one request; `0` disables. |
 `--idle-timeout <s>` | `300` | Close a connection that sends no request for this long; `0` disables. |
 `--allow-all`, `--allow-uid`, `--allow-user`, `--allow-gid`, `--allow-group` | group `mpfshost` | See below. |
 `--allow-vfs-write` | off | Enable the virtual file system write command (MemProcFS `VMMDLL_VfsWriteU`). |
 `--socket-mode <octal>`, `--socket-group <name\|gid>` | `0660`, access group | Socket permissions. |

Opening a VM costs about 0.7 s of MemProcFS initialization. The handle cache means repeated commands against the
same VM pay this only once per idle window. A single MemProcFS call cannot be interrupted, so a request that runs out
of budget is reported only when that call returns. For a large guest, raise `--request-timeout` for `pool` and
`search`.

Logs go to `stderr`, one line per record: ISO-8601 UTC timestamp, level, `category: message`. `debug` adds a trace
of every request. Guest memory contents are never logged.

### Access control

By default only members of the local group `mpfshost` (and root) may connect. The socket is created `0660` and owned
by that group. Membership is checked against the connecting process's kernel group list, so supplementary
membership counts. A peer whose credentials cannot be read is refused. If the group does not exist, the host refuses
to start:

```sh
sudo groupadd --system mpfshost
sudo usermod -aG mpfshost <user>    # takes effect at the next login, or after 'newgrp mpfshost'
```

`--allow-uid`, `--allow-user`, `--allow-gid` and `--allow-group` **add** to the default group. `--allow-all` serves
every local user with a `0666` socket and logs a warning; combining it with an `--allow-*` flag is an error.

A refused client receives `ErrorCode::AccessDenied` carrying the policy text. If the kernel itself denies
`connect()`, the client explains why: it reports the socket's mode, owner and group, your session's group ids, and
the remedy (re-login, `newgrp`, or `usermod`).

### Using the client

Every command opens a short-lived connection to the host. Commands that work inside a guest take the VM name and bind
to it automatically.

 Command | Description |
---------|-------------|
 `--client list-targets` | List the VM targets the host can serve, with their PID and QMP socket. |
 `--client capabilities` | Show the host version and the request types it handles. |
 `--client bind <vm>` | Bind to one VM target and report the session. |
 `--client processes <vm>` | List processes; `--pid` or `--name` filter. |
 `--client processes-all <vm>` | List every process, including terminated ones; `--pid` or `--name` filter. |
 `--client proc-info <vm>` | Full record of one process, by `--pid` or `--name` (resolved by MemProcFS). |
 `--client modules <vm>` | Loaded modules of a process; `--counts`, `--debug`, `--version-info`. |
 `--client symbol <vm>` | Module base and, with `--symbol`, an export address. |
 `--client pdb-load <vm>` | Load the pdb of the module at `--address` and report its module name. |
 `--client pdb-symbol <vm>` | Resolve `--address` to a pdb symbol name and displacement in `--module`. |
 `--client pdb-address <vm>` | Resolve `--symbol` to its address in `--module`. |
 `--client pdb-type-size <vm>` | Show the byte size of `--type` in `--module`. |
 `--client pdb-type-child <vm>` | Show the byte offset of `--child` in `--type` of `--module`. |
 `--client sections <vm>` | Section headers and 16 data directories of a module. |
 `--client maps <vm>` | VAD regions of a process; `--identify-modules` names owners. |
 `--client vadex <vm>` | Per-page VAD view of a process; `--page-from`/`--page-count`. |
 `--client threads <vm>` | Threads of a process; `--tid`, `--callstack`. |
 `--client handles <vm>` | Kernel handles of a process; `--name` filters names and types. |
 `--client eat <vm>` / `iat <vm>` | Export / import table of `--module`. |
 `--client unloaded <vm>` | Unloaded modules of a process. |
 `--client heap <vm>` | Heaps of a process and the number `heap-alloc` selects. |
 `--client heap-alloc <vm>` | Allocations of one heap; `--heap <number\|0xaddress>`. |
 `--client services <vm>` / `users <vm>` / `net <vm>` | Services, local accounts, network connections (`--pid` scopes `net`). |
 `--client kdriver <vm>` / `kdevice <vm>` / `kobject <vm>` | Kernel drivers, device objects, object namespace. |
 `--client pte <vm>` | Page-table regions of a process; `--identify-modules`, `--name`, `--min`/`--max`. |
 `--client pool <vm>` | Kernel pool allocations; `--tag`, `--min-size`, `--big`, `--min`/`--max`. |
 `--client pool-tags <vm>` | Kernel pool allocations summarized per tag; `--tag`, `--big`. |
 `--client pfn <vm>` | Page frames; `--pfn-from`, `--pfn-count` (≤ 32768). |
 `--client config-set <vm>` | Set an allow-listed MemProcFS cache/tick, refresh or process-DTB option; `--option`, `--value`, `--pid` where the option is process-scoped. |
 `--client read <vm>` / `write <vm>` | Process virtual memory read (hex dump) / write. |
 `--client scatter <vm>` | Several process ranges in one batch; repeat `--range`; `--repeat <n>` re-reads the prepared ranges, `--clear` clears the handle before closing. |
 `--client vquery <vm>` | Region, protection and type of a virtual address. |
 `--client page <vm>` / `prefetch <vm>` | Read whole 4 KiB pages / warm the page cache for `--range` lists (physical with `--pid -1`). |
 `--client search <vm>` | Search a process (or physical memory with `--pid -1`) for `--pattern` or `--hex-pattern`. |
 `--client vm-list <vm>` | Guest partitions; index `0` is the target's own physical memory. |
 `--client phys-read <vm>` / `phys-write <vm>` | Guest physical memory by GPA. |
 `--client vm-translate <vm>` / `virt2phys <vm>` | GPA → PA, process VA → PA. |
 `--client phys-scatter <vm>` | Several physical ranges in one batch. |
 `--client vfs-list <vm>` / `vfs-read <vm>` / `vfs-write <vm>` | MemProcFS virtual file system listing, file read, and (with `--allow-vfs-write`) file write. |

Common options:

 Option | Meaning |
--------|---------|
 `--socket <path>` | Socket to connect to (default `/run/mpfshost.sock`). |
 `--pid <pid>` | Target process. `-1` selects the bound target's physical memory for `page`, `prefetch` and `search`. For `net` it scopes the list; for `pte`, `0` describes the kernel's own tables. |
 `--name <text>` | Case-insensitive substring filter (for `proc-info`: the name to resolve). |
 `--module <name>` | Module for `symbol`, `sections`, `eat`, `iat`, `pdb-symbol`, `pdb-address`, `pdb-type-size`, `pdb-type-child`. |
 `--symbol <name>`, `--type <name>`, `--child <name>` | Symbol for `pdb-address`; struct type for `pdb-type-size` / `pdb-type-child`; child member for `pdb-type-child`. |
 `--address <hex>`, `--size <n>`, `--data <hex bytes>` | Address, decimal byte count and bytes for read/write commands; `pdb-load` takes the module base in `--address`. |
 `--range <hex>:<size>` | Repeatable range for `scatter`, `phys-scatter`, `page`, `prefetch`. |
 `--repeat <n>`, `--clear` | Repeat a `scatter` read (`n` rounds) and/or clear the handle before closing. |
 `--vm <index>` | Partition from `vm-list` (default `0`). |
 `--path <path>`, `--offset <hex>` | VFS path (`/` and `\` are equivalent) and read or write offset. |
 `--min <hex>`, `--max <hex>` | Address window; `--max` is exclusive. |
 `--page-from <n>`, `--page-count <n>` | Page window for `vadex` (count ≤ 4096). |
 `--option <name>`, `--value <n>` | Config option and decimal/`0x` value for `config-set`; `--value` is omitted for refresh triggers, `--pid` is required for process-scoped options. |
 `--pattern`, `--hex-pattern`, `--align`, `--max-results` | Search term (text, or hex with `??` wildcards, ≤ 32 bytes), hit alignment (power of two ≤ 4096), hit cap (default 1024, ≤ 65536). |

Run `./build/mpfshost --help` for the complete per-command option list.

Example session against VM `win11`, with the guest probe printing `PROBE pid=1234 address=0x123000 ...`:

```sh
./build/mpfshost --client list-targets
./build/mpfshost --client processes win11 --name guest-probe
./build/mpfshost --client modules win11 --pid 1234 --counts --debug
./build/mpfshost --client read win11 --pid 1234 --address 0x123000 --size 64
./build/mpfshost --client write win11 --pid 1234 --address 0x123000 --data "de ad be ef"
./build/mpfshost --client scatter win11 --pid 1234 --range 0x123000:64 --range 0x7ff00000:16
./build/mpfshost --client search win11 --pid 1234 --hex-pattern "de ad be ef" --max-results 16
./build/mpfshost --client virt2phys win11 --pid 1234 --address 0x123000
./build/mpfshost --client phys-read win11 --vm 0 --address 0x1a0000 --size 64
./build/mpfshost --client vfs-list win11 --path /misc/procinfo/
./build/mpfshost --client vfs-read win11 --path '\misc\procinfo\dtb.txt' --size 128
./build/mpfshost --client vfs-write win11 --path '\misc\custom\file.bin' --data "de ad be ef"
```

#### Things worth knowing

- Lists end with a summary line. `truncated` means a size cap cut the answer short. It is not an error.
- An empty result is a success: `no matching ...`, exit status `0`. A missing process, module, export, address region
  or nested VFS path is reported as an error, exit status `1`.
- Numbers are shown as MemProcFS reports them. Service `startType`/`state` are Windows `SERVICE_*` values. `net`
  states are MemProcFS's own (`0` closed, `1` listening, `4` established).
- Whether a map is populated depends on the guest. For example, the service map is empty on Windows 11 24H2.
- `page` marks each unreadable page `failed` rather than failing the request. One request is capped at 4096 pages,
  `prefetch` at 65536.
- `search` cannot match a pattern made only of zero bytes, so it rejects one.
- `--pid -1` makes `page`, `prefetch` and `search` address the target's physical memory (MemProcFS `dwPID = -1`);
  the other commands still need a real process id, because MemProcFS has no physical branch for a virtual query.
- A physical write must fit in one 1 MiB frame. Split larger writes.
- A `vfs-read` past the end of a file prints nothing and succeeds. A `vfs-list` of a misspelled *top-level*
  directory lists as empty, because MemProcFS cannot tell it apart from a real one.
- `vfs-write` is disabled unless the host was started with `--allow-vfs-write`; the host logs a warning when it is
  enabled, and the client receives `AccessDenied` otherwise. The guest file system must support writes.

### Using the client library

```cpp
#include <core/client/client.hpp>

std::string error;
const auto targets = mpfshost::listTargets("/run/mpfshost.sock", error);
```

```sh
g++ -std=c++23 myclient.cpp $(pkg-config --cflags --libs mpfshost) -o myclient
# or: -I<prefix>/include/mpfshost -L<prefix>/lib* -lmpfshost_core
```

The library has no stable ABI. Build your client with the same C++23 toolchain and standard library as the host.

Runnable programs covering target discovery, process listing, virtual memory reads, scatter reads and VFS listing
live in [`examples/`](examples/README.md). They are built by default (`-DMPFSHOST_BUILD_EXAMPLES=OFF` disables them)
and land in `build/examples/`.

### How targets are discovered

MemProcFS cannot enumerate QEMU/KVM guests, so the host pairs two files per VM: `/run/libvirt/qemu/<vm>.pid` (the
QEMU process id) and `/var/qmp-sockets/<vm>.sock` (its QMP socket). A VM is a target only when both exist. The device
string is `qemu://hugepage-pid=<pid>,qmp=<socket>`.

### Guest probe

[`tools/guest-probe/main.c`](tools/guest-probe/main.c) is a small Windows program that gives the host a known memory
target. When `x86_64-w64-mingw32-g++` is available, the build produces `build/guest/guest-probe.exe`. Copy it into the
guest and run:

```sh
guest-probe.exe [--size <bytes>] [--magic <hex byte>] [--watch]
```

The probe prints `PROBE pid=<pid> address=0x<address> size=<bytes> magic=0x<byte>` and then idles. Use that pid and
address with `read`, `write` or `scatter`. With `--watch`, it prints `CHANGE offset=0x.. old=0x.. new=0x..` for every
byte changed from outside, which confirms a write from inside the guest.

### Contributing

Read [`AGENTS.md`](AGENTS.md) first. In short: C++23, Ninja, warning-free build, `clang-format -i` on changed files,
`ctest` before committing, and leave `MemProcFS/` and `reference/` alone.
