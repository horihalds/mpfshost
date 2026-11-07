#include <catch2/catch.hpp>

#include "core/client/client.hpp"
#include "core/ipc/frame.hpp"
#include "core/ipc/message.hpp"
#include "core/server/server.hpp"
#include "support/fake_backend.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

TEST_CASE("vfs list request round trips", "[vfs][message]")
{
    const mpfshost::VfsListRequest request {"/misc/procinfo/", "dtb"};
    const auto                     decoded = mpfshost::decodeVfsListRequest(mpfshost::encodeVfsListRequest(request));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->path == "/misc/procinfo/");
    REQUIRE(decoded->nameFilter == "dtb");

    const mpfshost::VfsListRequest empty;
    const auto                     emptyDecoded = mpfshost::decodeVfsListRequest(mpfshost::encodeVfsListRequest(empty));
    REQUIRE(emptyDecoded.has_value());
    REQUIRE(emptyDecoded->path.empty());
    REQUIRE(emptyDecoded->nameFilter.empty());
}

TEST_CASE("vfs list data round trips", "[vfs][message]")
{
    const mpfshost::VfsListData empty;
    const auto                  emptyDecoded = mpfshost::decodeVfsListData(mpfshost::encodeVfsListData(empty));
    REQUIRE(emptyDecoded.has_value());
    REQUIRE(emptyDecoded->entries.empty());

    mpfshost::VfsListData data;
    data.entries.push_back({"dtb.txt", 12345, 0x1122334455667788ull, 1, 2, false});
    data.entries.push_back({"proc", 0, 0, 0, 0, true});
    data.entries.push_back({"na\u00efve", 7, 0, 0, 0, false});

    const auto decoded = mpfshost::decodeVfsListData(mpfshost::encodeVfsListData(data));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->entries.size() == 3);
    REQUIRE(decoded->entries[0].name == "dtb.txt");
    REQUIRE(decoded->entries[0].size == 12345);
    REQUIRE(decoded->entries[0].creationTime == 0x1122334455667788ull);
    REQUIRE(decoded->entries[0].lastAccessTime == 1);
    REQUIRE(decoded->entries[0].lastWriteTime == 2);
    REQUIRE_FALSE(decoded->entries[0].isDirectory);
    REQUIRE(decoded->entries[1].name == "proc");
    REQUIRE(decoded->entries[1].size == 0);
    REQUIRE(decoded->entries[1].isDirectory);
    REQUIRE(decoded->entries[2].name == "na\u00efve");
}

TEST_CASE("vfs list end round trips", "[vfs][message]")
{
    const mpfshost::VfsListEnd ok {0, 42, ""};
    const auto                 okDecoded = mpfshost::decodeVfsListEnd(mpfshost::encodeVfsListEnd(ok));
    REQUIRE(okDecoded.has_value());
    REQUIRE(okDecoded->status == 0);
    REQUIRE(okDecoded->entryCount == 42);
    REQUIRE(okDecoded->error.empty());

    const mpfshost::VfsListEnd failed {1, 0, "vfs list failed"};
    const auto                 failedDecoded = mpfshost::decodeVfsListEnd(mpfshost::encodeVfsListEnd(failed));
    REQUIRE(failedDecoded.has_value());
    REQUIRE(failedDecoded->status == 1);
    REQUIRE(failedDecoded->error == "vfs list failed");
}

TEST_CASE("vfs read request round trips", "[vfs][message]")
{
    const mpfshost::VfsReadRequest request {"\\misc\\procinfo\\dtb.txt", 0x1000, 4096};
    const auto                     decoded = mpfshost::decodeVfsReadRequest(mpfshost::encodeVfsReadRequest(request));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->path == "\\misc\\procinfo\\dtb.txt");
    REQUIRE(decoded->offset == 0x1000);
    REQUIRE(decoded->size == 4096);
}

TEST_CASE("vfs read end round trips", "[vfs][message]")
{
    const mpfshost::VfsReadEnd ok {0, 1024, ""};
    const auto                 okDecoded = mpfshost::decodeVfsReadEnd(mpfshost::encodeVfsReadEnd(ok));
    REQUIRE(okDecoded.has_value());
    REQUIRE(okDecoded->status == 0);
    REQUIRE(okDecoded->bytesRead == 1024);
    REQUIRE(okDecoded->error.empty());

    const mpfshost::VfsReadEnd failed {1, 0, "path does not exist"};
    const auto                 failedDecoded = mpfshost::decodeVfsReadEnd(mpfshost::encodeVfsReadEnd(failed));
    REQUIRE(failedDecoded.has_value());
    REQUIRE(failedDecoded->status == 1);
    REQUIRE(failedDecoded->error == "path does not exist");
}

TEST_CASE("vfs write request round trips", "[vfs][message]")
{
    const std::vector<std::byte>    data {std::byte {0xDE}, std::byte {0xAD}, std::byte {0xBE}, std::byte {0xEF}};
    const mpfshost::VfsWriteRequest request {"\\misc\\custom\\file.bin", 0x2000, data};
    const auto                      decoded = mpfshost::decodeVfsWriteRequest(mpfshost::encodeVfsWriteRequest(request));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->path == "\\misc\\custom\\file.bin");
    REQUIRE(decoded->offset == 0x2000);
    REQUIRE(decoded->data == data);

    const mpfshost::VfsWriteRequest empty {"\\misc\\custom\\file.bin", 0, {}};
    const auto emptyDecoded = mpfshost::decodeVfsWriteRequest(mpfshost::encodeVfsWriteRequest(empty));
    REQUIRE(emptyDecoded.has_value());
    REQUIRE(emptyDecoded->data.empty());
}

TEST_CASE("vfs write response round trips", "[vfs][message]")
{
    const mpfshost::VfsWriteResponse response {42};
    const auto decoded = mpfshost::decodeVfsWriteResponse(mpfshost::encodeVfsWriteResponse(response));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->bytesWritten == 42);
}

TEST_CASE("malformed vfs payloads are rejected", "[vfs][message]")
{
    const std::vector<std::byte> empty;
    REQUIRE_FALSE(mpfshost::decodeVfsListRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeVfsListData(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeVfsListEnd(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeVfsReadRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeVfsReadEnd(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeVfsWriteRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeVfsWriteResponse(empty).has_value());

    mpfshost::ByteWriter oversizedString;
    oversizedString.writeU32(16);
    oversizedString.writeU8(0);
    REQUIRE_FALSE(mpfshost::decodeVfsListRequest(oversizedString.bytes()).has_value());
    REQUIRE_FALSE(mpfshost::decodeVfsReadRequest(oversizedString.bytes()).has_value());

    mpfshost::ByteWriter trailingData;
    trailingData.writeU32(0);
    trailingData.writeU8(1);
    REQUIRE_FALSE(mpfshost::decodeVfsListData(trailingData.bytes()).has_value());

    mpfshost::ByteWriter absurdCount;
    absurdCount.writeU32(0xFFFFFFFFu);
    REQUIRE_FALSE(mpfshost::decodeVfsListData(absurdCount.bytes()).has_value());

    mpfshost::VfsListData data;
    data.entries.push_back({"alpha", 1, 0, 0, 0, false});
    auto listBytes = mpfshost::encodeVfsListData(data);
    listBytes.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeVfsListData(listBytes).has_value());

    auto readEndBytes = mpfshost::encodeVfsReadEnd({0, 1, "x"});
    readEndBytes.pop_back();
    REQUIRE_FALSE(mpfshost::decodeVfsReadEnd(readEndBytes).has_value());

    auto writeResponseBytes = mpfshost::encodeVfsWriteResponse({1});
    writeResponseBytes.pop_back();
    REQUIRE_FALSE(mpfshost::decodeVfsWriteResponse(writeResponseBytes).has_value());

    auto writeRequestBytes = mpfshost::encodeVfsWriteRequest({"\\misc\\a", 0, {std::byte {1}}});
    writeRequestBytes.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeVfsWriteRequest(writeRequestBytes).has_value());
}

namespace
{
    class VfsServerFixture
    {
    public:
        VfsServerFixture()
        {
            std::filesystem::create_directories(MPFSHOST_TEST_TMP_DIR);
            std::string pattern = (std::filesystem::path {MPFSHOST_TEST_TMP_DIR} / "vfs-XXXXXX").string();
            REQUIRE(::mkdtemp(pattern.data()) != nullptr);
            _root       = pattern;
            _socketPath = _root / "host.sock";
            REQUIRE(_socketPath.string().size() < sizeof(sockaddr_un::sun_path));
        }

        ~VfsServerFixture()
        {
            std::error_code error;
            std::filesystem::remove_all(_root, error);
        }

        VfsServerFixture(const VfsServerFixture&)            = delete;
        VfsServerFixture& operator=(const VfsServerFixture&) = delete;

        [[nodiscard]] const std::filesystem::path& root() const noexcept
        {
            return _root;
        }

        [[nodiscard]] const std::filesystem::path& socketPath() const noexcept
        {
            return _socketPath;
        }

    private:
        std::filesystem::path _root;
        std::filesystem::path _socketPath;
    };

    int connectTo(const std::filesystem::path& path)
    {
        const auto text = path.string();
        for (int attempt = 0; attempt < 400; ++attempt)
        {
            const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd < 0)
            {
                return -1;
            }

            sockaddr_un address {};
            address.sun_family = AF_UNIX;
            std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
            if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0)
            {
                return fd;
            }
            ::close(fd);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return -1;
    }

    class RunningServer
    {
    public:
        RunningServer(const std::filesystem::path&  socketPath,
                      const mpfshost::TargetSource& targets,
                      mpfshost::VmBackend&          backend)
            : RunningServer(mpfshost::ServerOptions {socketPath}, targets, backend)
        {
        }

        RunningServer(mpfshost::ServerOptions       options,
                      const mpfshost::TargetSource& targets,
                      mpfshost::VmBackend&          backend)
            : _server {std::move(options), targets, backend},
              _thread {[this]
                       {
                           std::string error;
                           if (!_server.run(error))
                           {
                               std::fprintf(stderr, "server failed to start: %s\n", error.c_str());
                           }
                       }}
        {
        }

        ~RunningServer()
        {
            _server.requestStop();
            _thread.join();
        }

        RunningServer(const RunningServer&)            = delete;
        RunningServer& operator=(const RunningServer&) = delete;

    private:
        mpfshost::Server _server;
        std::jthread     _thread;
    };

    class RawClient
    {
    public:
        explicit RawClient(const std::filesystem::path& socketPath)
        {
            _fd = connectTo(socketPath);
            REQUIRE(_fd >= 0);
        }

        ~RawClient()
        {
            if (_fd >= 0)
            {
                ::close(_fd);
            }
        }

        RawClient(const RawClient&)            = delete;
        RawClient& operator=(const RawClient&) = delete;

        void send(mpfshost::MessageType type, std::uint64_t requestId, std::span<const std::byte> payload = {}) const
        {
            std::string error;
            REQUIRE(mpfshost::writeFrame(_fd, type, requestId, payload, error));
        }

        bool receive(mpfshost::MessageType& type, std::uint64_t& requestId, std::vector<std::byte>& payload) const
        {
            mpfshost::FrameHeader header;
            std::string           error;
            if (mpfshost::readFrame(_fd, header, payload, error) != mpfshost::FrameReadStatus::Complete)
            {
                return false;
            }
            type      = header.type;
            requestId = header.requestId;
            return true;
        }

    private:
        int _fd {-1};
    };

    void bindClient(RawClient& client, std::uint64_t requestId, const std::string& targetName)
    {
        client.send(mpfshost::MessageType::BindRequest, requestId, mpfshost::encodeBindRequest({targetName}));

        mpfshost::MessageType  type;
        std::uint64_t          responseId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, responseId, payload));
        REQUIRE(type == mpfshost::MessageType::BindResponse);
    }
} // namespace

TEST_CASE("vfs name filter matches case-insensitively", "[vfs]")
{
    const mpfshost::VfsListRequest request {"/misc", ""};
    const mpfshost::VfsEntry       file {"dtb.txt", 10, 0, 0, 0, false};
    const mpfshost::VfsEntry       directory {"proc", 0, 0, 0, 0, true};

    REQUIRE(mpfshost::matchesVfsFilter(request, file));
    REQUIRE(mpfshost::matchesVfsFilter(request, directory));

    const mpfshost::VfsListRequest named {"/misc", "DTB"};
    REQUIRE(mpfshost::matchesVfsFilter(named, file));
    REQUIRE_FALSE(mpfshost::matchesVfsFilter(named, directory));

    const mpfshost::VfsListRequest unmatched {"/misc", "zzz"};
    REQUIRE_FALSE(mpfshost::matchesVfsFilter(unmatched, file));
}

TEST_CASE("server streams a vfs listing in batches", "[vfs][server]")
{
    VfsServerFixture                    fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;

    std::vector<mpfshost::VfsEntry> entries;
    for (std::uint32_t index = 0; index < 300; ++index)
    {
        entries.push_back({"entry" + std::to_string(index) + std::string(2048, 'x'), index, 0, 0, 0, index % 2 == 0});
    }
    backend.setVfsEntries(entries);
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    RunningServer server {fixture.socketPath(), targets, backend};

    RawClient client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    const mpfshost::VfsListRequest request {"/misc/procinfo/", ""};
    client.send(mpfshost::MessageType::VfsListRequest, 2, mpfshost::encodeVfsListRequest(request));

    std::uint64_t total  = 0;
    bool          sawEnd = false;
    while (!sawEnd)
    {
        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(requestId == 2);
        if (type == mpfshost::MessageType::VfsListData)
        {
            const auto data = mpfshost::decodeVfsListData(payload);
            REQUIRE(data.has_value());
            total += data->entries.size();
            continue;
        }

        REQUIRE(type == mpfshost::MessageType::VfsListEnd);
        const auto end = mpfshost::decodeVfsListEnd(payload);
        REQUIRE(end.has_value());
        REQUIRE(end->status == 0);
        REQUIRE(end->entryCount == entries.size());
        sawEnd = true;
    }

    REQUIRE(total == entries.size());
    const auto last = backend.vfsState()->lastListRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->path == "/misc/procinfo/");
}

TEST_CASE("server returns a short vfs read with status zero", "[vfs][server]")
{
    VfsServerFixture                    fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;

    const std::vector<std::byte> fileBytes {std::byte {0x10},
                                            std::byte {0x11},
                                            std::byte {0x12},
                                            std::byte {0x13},
                                            std::byte {0x14},
                                            std::byte {0x15},
                                            std::byte {0x16},
                                            std::byte {0x17},
                                            std::byte {0x18},
                                            std::byte {0x19}};
    backend.setVfsFile("\\misc\\procinfo\\dtb.txt", fileBytes);
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    RunningServer server {fixture.socketPath(), targets, backend};

    RawClient client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    const mpfshost::VfsReadRequest request {"\\misc\\procinfo\\dtb.txt", 4, 100};
    client.send(mpfshost::MessageType::VfsReadRequest, 2, mpfshost::encodeVfsReadRequest(request));

    std::vector<std::byte> bytes;
    bool                   sawEnd = false;
    while (!sawEnd)
    {
        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(requestId == 2);
        if (type == mpfshost::MessageType::VfsReadData)
        {
            bytes.insert(bytes.end(), payload.begin(), payload.end());
            continue;
        }

        REQUIRE(type == mpfshost::MessageType::VfsReadEnd);
        const auto end = mpfshost::decodeVfsReadEnd(payload);
        REQUIRE(end.has_value());
        REQUIRE(end->status == 0);
        REQUIRE(end->bytesRead == 6);
        sawEnd = true;
    }

    REQUIRE(bytes.size() == 6);
    REQUIRE(bytes.front() == std::byte {0x14});
    REQUIRE(bytes.back() == std::byte {0x19});

    const auto last = backend.vfsState()->lastReadRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->path == "\\misc\\procinfo\\dtb.txt");
    REQUIRE(last->offset == 4);
    REQUIRE(last->size == 100);
}

TEST_CASE("server rejects a vfs request on an unbound session", "[vfs][server]")
{
    VfsServerFixture                    fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    RunningServer                       server {fixture.socketPath(), targets, backend};

    RawClient client {fixture.socketPath()};
    client.send(mpfshost::MessageType::VfsListRequest, 1, mpfshost::encodeVfsListRequest({"/misc", ""}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::NotBound);
}

TEST_CASE("server reports a missing vfs capability", "[vfs][server]")
{
    VfsServerFixture                    fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    backend.setHandlesBare(true);
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    RunningServer server {fixture.socketPath(), targets, backend};

    RawClient client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::VfsListRequest, 2, mpfshost::encodeVfsListRequest({"/misc", ""}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::CapabilityUnavailable);

    client.send(
        mpfshost::MessageType::VfsReadRequest, 3, mpfshost::encodeVfsReadRequest({"\\misc\\procinfo\\dtb.txt", 0, 16}));

    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto readError = mpfshost::decodeErrorResponse(payload);
    REQUIRE(readError.has_value());
    REQUIRE(readError->code == mpfshost::ErrorCode::CapabilityUnavailable);
}

TEST_CASE("server rejects a malformed vfs payload", "[vfs][server]")
{
    VfsServerFixture                    fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    RunningServer server {fixture.socketPath(), targets, backend};

    RawClient client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    mpfshost::ByteWriter malformed;
    malformed.writeU32(64);
    malformed.writeU8(0);
    client.send(mpfshost::MessageType::VfsListRequest, 2, malformed.bytes());

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::MalformedPayload);
}

TEST_CASE("server rejects invalid vfs read sizes", "[vfs][server]")
{
    VfsServerFixture                    fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    RunningServer server {fixture.socketPath(), targets, backend};

    RawClient client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;

    client.send(
        mpfshost::MessageType::VfsReadRequest, 2, mpfshost::encodeVfsReadRequest({"\\misc\\procinfo\\dtb.txt", 0, 0}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::InvalidRange);

    client.send(mpfshost::MessageType::VfsReadRequest,
                3,
                mpfshost::encodeVfsReadRequest({"\\misc\\procinfo\\dtb.txt", 0, mpfshost::kMaxReadSize + 1}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::InvalidRange);
}

TEST_CASE("server refuses vfs writes unless the host opted in", "[vfs][server]")
{
    VfsServerFixture                    fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    backend.setVfsFile("\\misc\\custom\\file.bin", {std::byte {0}});
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    RunningServer server {fixture.socketPath(), targets, backend};

    RawClient client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    const std::vector<std::byte> data {std::byte {0xAA}, std::byte {0xBB}};
    client.send(mpfshost::MessageType::VfsWriteRequest,
                2,
                mpfshost::encodeVfsWriteRequest({"\\misc\\custom\\file.bin", 0, data}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::AccessDenied);
    REQUIRE_FALSE(backend.vfsState()->lastWriteRequest.has_value());
}

TEST_CASE("server writes a vfs window when the host opted in", "[vfs][server]")
{
    VfsServerFixture                    fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    backend.setVfsFile("\\misc\\custom\\file.bin",
                       {std::byte {1}, std::byte {2}, std::byte {3}, std::byte {4}, std::byte {5}});
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::ServerOptions options {fixture.socketPath()};
    options.allowVfsWrite = true;
    RunningServer server {options, targets, backend};

    RawClient client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    const std::vector<std::byte> data {std::byte {0xAA}, std::byte {0xBB}, std::byte {0xCC}};
    client.send(mpfshost::MessageType::VfsWriteRequest,
                2,
                mpfshost::encodeVfsWriteRequest({"\\misc\\custom\\file.bin", 1, data}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::VfsWriteResponse);
    const auto response = mpfshost::decodeVfsWriteResponse(payload);
    REQUIRE(response.has_value());
    REQUIRE(response->bytesWritten == data.size());

    const auto last = backend.vfsState()->lastWriteRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->path == "\\misc\\custom\\file.bin");
    REQUIRE(last->offset == 1);
    REQUIRE(last->data == data);
}

TEST_CASE("server reports a missing vfs write capability", "[vfs][server]")
{
    VfsServerFixture                    fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    backend.setHandlesBare(true);
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::ServerOptions options {fixture.socketPath()};
    options.allowVfsWrite = true;
    RunningServer server {options, targets, backend};

    RawClient client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::VfsWriteRequest,
                2,
                mpfshost::encodeVfsWriteRequest({"\\misc\\custom\\file.bin", 0, {std::byte {0xAA}}}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::CapabilityUnavailable);
}

TEST_CASE("server rejects an empty vfs write", "[vfs][server]")
{
    VfsServerFixture                    fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::ServerOptions options {fixture.socketPath()};
    options.allowVfsWrite = true;
    RunningServer server {options, targets, backend};

    RawClient client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::VfsWriteRequest,
                2,
                mpfshost::encodeVfsWriteRequest({"\\misc\\custom\\file.bin", 0, {}}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::InvalidRange);
}

namespace
{
    // Answers one bind and one VFS list, claiming two entries while delivering only one.
    class MismatchedVfsServer
    {
    public:
        explicit MismatchedVfsServer(const std::filesystem::path& socketPath) : _socketPath {socketPath}
        {
            const auto text = socketPath.string();
            _listener       = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            REQUIRE(_listener >= 0);

            sockaddr_un address {};
            address.sun_family = AF_UNIX;
            std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
            REQUIRE(::bind(_listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
            REQUIRE(::listen(_listener, 1) == 0);

            _thread = std::jthread {[this]
                                    {
                                        serve();
                                    }};
        }

        ~MismatchedVfsServer()
        {
            _thread.join();
            ::close(_listener);
            std::error_code error;
            std::filesystem::remove(_socketPath, error);
        }

        MismatchedVfsServer(const MismatchedVfsServer&)            = delete;
        MismatchedVfsServer& operator=(const MismatchedVfsServer&) = delete;

    private:
        void serve()
        {
            const int fd = ::accept(_listener, nullptr, nullptr);
            if (fd < 0)
            {
                return;
            }

            std::string            error;
            mpfshost::FrameHeader  header;
            std::vector<std::byte> payload;
            if (mpfshost::readFrame(fd, header, payload, error) == mpfshost::FrameReadStatus::Complete)
            {
                const mpfshost::BindResponse response {
                    7, {"vm-alpha", 111, "/tmp/qmp.sock"}
                };
                (void)mpfshost::writeFrame(fd,
                                           mpfshost::MessageType::BindResponse,
                                           header.requestId,
                                           mpfshost::encodeBindResponse(response),
                                           error);
            }
            if (mpfshost::readFrame(fd, header, payload, error) == mpfshost::FrameReadStatus::Complete)
            {
                const mpfshost::VfsListData data {{{"only.txt", 1, 0, 0, 0, false}}};
                (void)mpfshost::writeFrame(
                    fd, mpfshost::MessageType::VfsListData, header.requestId, mpfshost::encodeVfsListData(data), error);
                const mpfshost::VfsListEnd end {0, 2, ""};
                (void)mpfshost::writeFrame(
                    fd, mpfshost::MessageType::VfsListEnd, header.requestId, mpfshost::encodeVfsListEnd(end), error);
            }
            ::close(fd);
        }

        std::filesystem::path _socketPath;
        int                   _listener {-1};
        std::jthread          _thread;
    };
} // namespace

TEST_CASE("client lists the vfs and filters by name", "[vfs][client]")
{
    VfsServerFixture                    fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    backend.setVfsEntries({
        {"dtb.txt", 128, 0, 0, 0, false},
        {   "proc",   0, 0, 0, 0,  true}
    });
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  entries = mpfshost::listVfs(fixture.socketPath(), {"vm-alpha", "/misc/procinfo/", ""}, error);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 2);
    REQUIRE((*entries)[0].name == "dtb.txt");
    REQUIRE((*entries)[0].size == 128);
    REQUIRE_FALSE((*entries)[0].isDirectory);
    REQUIRE((*entries)[1].isDirectory);

    const auto filtered = mpfshost::listVfs(fixture.socketPath(), {"vm-alpha", "/misc/procinfo/", "DTB"}, error);
    REQUIRE(filtered.has_value());
    REQUIRE(filtered->size() == 1);
    REQUIRE((*filtered)[0].name == "dtb.txt");
}

TEST_CASE("client reads a vfs window and treats eof as success", "[vfs][client]")
{
    VfsServerFixture                    fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;

    std::vector<std::byte> bytes(64);
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        bytes[index] = static_cast<std::byte>(index);
    }
    backend.setVfsFile("\\misc\\procinfo\\dtb.txt", bytes);
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  window =
        mpfshost::readVfs(fixture.socketPath(), {"vm-alpha", "\\misc\\procinfo\\dtb.txt", 16, 8}, error);
    REQUIRE(window.has_value());
    REQUIRE(window->size() == 8);
    REQUIRE((*window)[0] == std::byte {16});

    const auto past =
        mpfshost::readVfs(fixture.socketPath(), {"vm-alpha", "\\misc\\procinfo\\dtb.txt", 1024, 8}, error);
    REQUIRE(past.has_value());
    REQUIRE(past->empty());

    const auto missing =
        mpfshost::readVfs(fixture.socketPath(), {"vm-alpha", "\\misc\\procinfo\\missing.txt", 0, 8}, error);
    REQUIRE_FALSE(missing.has_value());
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("client rejects a vfs listing with a count mismatch", "[vfs][client]")
{
    VfsServerFixture    fixture;
    MismatchedVfsServer server {fixture.socketPath()};

    std::string error;
    const auto  entries = mpfshost::listVfs(fixture.socketPath(), {"vm-alpha", "\\", ""}, error);
    REQUIRE_FALSE(entries.has_value());
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("client surfaces a missing vfs path", "[vfs][client]")
{
    VfsServerFixture                    fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    backend.failVfsWith("vfs listing failed: path '\\misc\\missing' is not present");
    backend.vfsState()->status = mpfshost::LookupStatus::NotFound;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  entries = mpfshost::listVfs(fixture.socketPath(), {"vm-alpha", "\\misc\\missing", ""}, error);

    REQUIRE_FALSE(entries.has_value());
    REQUIRE(error == "vfs listing failed: path '\\misc\\missing' is not present");
}

TEST_CASE("client treats an existing empty vfs directory as success", "[vfs][client]")
{
    VfsServerFixture                    fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    backend.setVfsEntries({});
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  entries = mpfshost::listVfs(fixture.socketPath(), {"vm-alpha", "\\misc\\empty", ""}, error);

    REQUIRE(entries.has_value());
    REQUIRE(entries->empty());
    REQUIRE(error.empty());
}

TEST_CASE("client writes a vfs window", "[vfs][client]")
{
    VfsServerFixture                    fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    backend.setVfsFile("\\misc\\custom\\file.bin", {std::byte {0}, std::byte {0}, std::byte {0}, std::byte {0}});
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::ServerOptions options {fixture.socketPath()};
    options.allowVfsWrite = true;
    RunningServer server {options, targets, backend};

    std::string                  error;
    const std::vector<std::byte> data {std::byte {1}, std::byte {2}, std::byte {3}};
    const auto                   written =
        mpfshost::writeVfs(fixture.socketPath(), {"vm-alpha", "\\misc\\custom\\file.bin", 1, data}, error);

    REQUIRE(written.has_value());
    REQUIRE(*written == data.size());
    REQUIRE(error.empty());

    const auto file = backend.vfsState()->files.find("\\misc\\custom\\file.bin");
    REQUIRE(file != backend.vfsState()->files.end());
    REQUIRE(file->second.size() == 4);
    REQUIRE(file->second[1] == std::byte {1});
    REQUIRE(file->second[3] == std::byte {3});
}

TEST_CASE("client surfaces a vfs write failure", "[vfs][client]")
{
    VfsServerFixture                    fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    backend.failVfsWriteWith("write is not supported for this file");
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::ServerOptions options {fixture.socketPath()};
    options.allowVfsWrite = true;
    RunningServer server {options, targets, backend};

    std::string error;
    const auto  written =
        mpfshost::writeVfs(fixture.socketPath(), {"vm-alpha", "\\misc\\custom\\file.bin", 0, {std::byte {1}}}, error);

    REQUIRE_FALSE(written.has_value());
    REQUIRE(error == "write is not supported for this file");
}
