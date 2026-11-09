# Client API examples

Small programs that use the unprivileged `mpfshost` client library. They connect to a running host over its Unix
socket, so no root is needed. The user running them must be allowed by the host's access policy (by default, a member
of the `mpfshost` group).

| Example | Shows |
|---------|-------|
| `list_targets.cpp` | `Client::capabilities` and `Client::targets`. |
| `list_processes.cpp` | `Client::bind` and `ClientSession::processes` with an optional name filter. |
| `read_process_memory.cpp` | Reading and hex-dumping process virtual memory. |
| `scatter_read.cpp` | Reading several process ranges in one batched request. |
| `list_vfs.cpp` | Listing a directory of the MemProcFS virtual file system. |

Every example opens one client, binds one session, and keeps that connection for all calls.

## Build in the source tree

Examples are built by default (`-DMPFSHOST_BUILD_EXAMPLES=OFF` disables them):

```sh
cmake -S . -B build -G Ninja
ninja -C build
```

The binaries land in `build/examples/`.

## Build against an installed tree

An installed host also ships this README and the example sources in `<prefix>/share/doc/mpfshost/examples`; build
them from there.

```sh
c++ -std=c++23 $(pkg-config --cflags mpfshost) list_targets.cpp \
    $(pkg-config --libs mpfshost) -o list_targets
```

or without pkg-config:

```sh
c++ -std=c++23 -I<prefix>/include -I<prefix>/include/mpfshost list_targets.cpp \
    -L<prefix>/lib* -lmpfshost_client -lmpfshost_ipc -lmpfshost_util -pthread -o list_targets
```

With CMake use `find_package(mpfshost CONFIG REQUIRED)` and link `mpfshost::client`.

## Run

The first argument is the host socket. Replace `/run/mpfshost.sock` with the host's `--socket` path if you changed it,
and `win11` with a target name from `list-targets`.

```sh
./build/examples/mpfshost_example_list_targets /run/mpfshost.sock
./build/examples/mpfshost_example_list_processes /run/mpfshost.sock win11
./build/examples/mpfshost_example_list_processes /run/mpfshost.sock win11 notepad
./build/examples/mpfshost_example_read_process_memory /run/mpfshost.sock win11 1234 0x7ff6a0000000 64
./build/examples/mpfshost_example_scatter_read /run/mpfshost.sock win11 1234 0x7ff6a0000000:16 0x7ff6a0001000:16
./build/examples/mpfshost_example_list_vfs /run/mpfshost.sock win11 /
```

Each program prints an error to `stderr` and exits non-zero when a request fails.
