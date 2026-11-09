# Client library

`<mpfshost/client.hpp>` is the public interface for unprivileged applications. It talks to the root host over the
host's Unix socket and never loads `vmm.so`, `leechcore.so`, MemProcFS headers or root-only code.

## Object model

```cpp
mpfshost::Client client;                 // configured with /run/mpfshost.sock
mpfshost::Client client {"/tmp/custom.sock"};
mpfshost::Client guarded {"/run/mpfshost.sock", std::chrono::seconds {10}}; // I/O timeout

auto capabilities = client.capabilities();
auto targets      = client.targets();
auto session      = client.bind("win11"); // Result<ClientSession>
```

- `Client` holds an endpoint, not a connection. `capabilities` and `targets` open a short-lived connection each.
- `ClientSession` owns one connection already bound to one target. Every method on it uses that connection and
  takes no `targetName`.
- `Scatter` is returned by `ClientSession::scatter(pid)` and shares the session's connection. It keeps that connection
  alive by itself, so it is safe to move a scatter object out of the session's lifetime.
- `ClientSession` and `Scatter` are move-only. Both close deterministically.

## Results and errors

Every fallible call returns `mpfshost::Result<T>`, a `std::expected<T, mpfshost::Error>`.

```cpp
auto data = session->readMemory(pid, address, size);
if (!data)
{
    if (data.error().code == mpfshost::ErrorCode::NotFound)
    {
        // ...
    }
    std::cerr << mpfshost::errorCodeName(data.error().code) << ": " << data.error().message << '\n';
    return 1;
}
```

`Error::code` carries the server's `ErrorCode` when the host rejected the request, or one of the client-side
values `ConnectionFailed`, `ProtocolError` or `InvalidArgument`. `Error::message` is the human-readable text.

List-shaped capabilities return `List<T>`:

```cpp
auto processes = session->processes({.nameFilter = "notepad"});
for (const auto& process : *processes) { /* ... */ }
if (processes->truncated) { /* the server cap cut the answer short */ }
```

`List<T>` has `items`, `truncated`, `size`, `empty`, `begin`, `end` and `operator[]`, so it works directly in a
range-for.

## Threading and connection lifetime

- `Client` is effectively immutable and can be shared.
- `ClientSession` is not thread-safe: one request at a time per connection, like the protocol. Create one session per
  thread, or serialize calls yourself.
- A session keeps a host worker thread occupied while the connection is open. Hosts are sized for a small number of
  concurrent clients (`--workers`, default 8). Keep sessions short-lived or budget for that occupancy. The host's
  `--idle-timeout` eventually closes abandoned connections.
- `ClientSession::close()` releases the connection immediately; the destructor does the same.

## Buffers and streaming

Convenience calls return owned buffers:

```cpp
auto bytes = session->readMemory(pid, address, 0x1000);
auto image = session->readVfs("\\Windows\\System32\\ntdll.dll", 0, 4096);
```

For repeated reads, use the span form so the caller owns the destination buffer:

```cpp
std::vector<std::byte> buffer(0x1000);
auto filled = session->readMemoryInto(pid, address, std::span<std::byte>(buffer));

// Tolerant form: returns the readable prefix, possibly zero bytes.
auto bytes = session->readMemoryPartial(pid, address, std::span<std::byte>(buffer));

auto written = session->writeMemory(pid, address, std::span<const std::byte>(buffer));
```

`Scatter` prepares a batch once and executes it repeatedly:

```cpp
auto scatter = session->scatter(pid);
auto prepared = scatter->prepare({{address, 16, false, {}}});
auto results  = scatter->execute();
```

## Discovery and feature checks

`Client::capabilities()` returns the host version and the request types it handles. Use
`Capabilities::supports(MessageType)` instead of inspecting the raw values.

## Compatibility

There is no stable client API or ABI. The host and the library are built and deployed together. Public headers,
function names, result types and library archives may change without compatibility shims.

## Examples

Runnable programs live in `examples/`. The installed tree ships them under
`<prefix>/share/doc/mpfshost/examples/`. They use one `ClientSession` per process and keep it for all calls.
