#include <catch2/catch.hpp>

#include "core/ipc/frame.hpp"
#include "core/ipc/message.hpp"
#include "core/log/log.hpp"
#include "core/server/handle_cache.hpp"
#include "core/server/server.hpp"
#include "support/fake_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace
{
    class ServerFixture
    {
    public:
        ServerFixture()
        {
            std::filesystem::create_directories(MPFSHOST_TEST_TMP_DIR);
            std::string pattern = (std::filesystem::path {MPFSHOST_TEST_TMP_DIR} / "server-XXXXXX").string();
            REQUIRE(::mkdtemp(pattern.data()) != nullptr);
            _root       = pattern;
            _socketPath = _root / "host.sock";
            REQUIRE(_socketPath.string().size() < sizeof(sockaddr_un::sun_path));
        }

        ~ServerFixture()
        {
            std::error_code error;
            std::filesystem::remove_all(_root, error);
        }

        ServerFixture(const ServerFixture&)            = delete;
        ServerFixture& operator=(const ServerFixture&) = delete;

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

    bool waitFor(const std::function<bool()>& predicate)
    {
        for (int attempt = 0; attempt < 400; ++attempt)
        {
            if (predicate())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    class ClientSocket
    {
    public:
        explicit ClientSocket(const std::filesystem::path& socketPath)
        {
            _fd = connectTo(socketPath);
            REQUIRE(_fd >= 0);
        }

        ~ClientSocket()
        {
            if (_fd >= 0)
            {
                ::close(_fd);
            }
        }

        ClientSocket(const ClientSocket&)            = delete;
        ClientSocket& operator=(const ClientSocket&) = delete;

        void send(mpfshost::MessageType type, std::uint64_t requestId, std::span<const std::byte> payload = {}) const
        {
            std::string error;
            REQUIRE(mpfshost::writeFrame(_fd, type, requestId, payload, error));
        }

        void sendRaw(std::span<const std::byte> bytes) const
        {
            REQUIRE(::write(_fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
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

        void close()
        {
            if (_fd >= 0)
            {
                ::close(_fd);
                _fd = -1;
            }
        }

        void setReceiveTimeout(std::chrono::milliseconds timeout) const
        {
            timeval value {};
            value.tv_sec  = static_cast<time_t>(timeout.count() / 1000);
            value.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
            REQUIRE(::setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) == 0);
        }

        [[nodiscard]] int fd() const noexcept
        {
            return _fd;
        }

    private:
        int _fd {-1};
    };

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

        [[nodiscard]] std::size_t activeSessions() const noexcept
        {
            return _server.activeSessions();
        }

        [[nodiscard]] std::size_t pendingConnections() const noexcept
        {
            return _server.pendingConnections();
        }

    private:
        mpfshost::Server _server;
        std::jthread     _thread;
    };

    std::vector<std::byte> rawFrame(std::uint32_t magic,
                                    std::uint16_t version,
                                    std::uint16_t type,
                                    std::uint64_t requestId,
                                    std::uint32_t payloadLength)
    {
        mpfshost::ByteWriter writer;
        writer.writeU32(magic);
        writer.writeU16(version);
        writer.writeU16(type);
        writer.writeU64(requestId);
        writer.writeU32(payloadLength);
        return writer.take();
    }

    void bindClient(ClientSocket& client, std::uint64_t requestId, const std::string& targetName)
    {
        client.send(mpfshost::MessageType::BindRequest, requestId, mpfshost::encodeBindRequest({targetName}));

        mpfshost::MessageType  type;
        std::uint64_t          responseId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, responseId, payload));
        REQUIRE(type == mpfshost::MessageType::BindResponse);
    }

    std::vector<std::byte> addressPattern(std::uint64_t address, std::size_t size)
    {
        std::vector<std::byte> pattern(size);
        for (std::size_t i = 0; i < size; ++i)
        {
            pattern[i] = static_cast<std::byte>((address + i) & 0xFF);
        }
        return pattern;
    }

    class LogCapture
    {
    public:
        LogCapture()
        {
            _previousLevel = mpfshost::log::level();
            mpfshost::log::setLevel(mpfshost::log::Level::Debug);
            mpfshost::log::setSink(
                [this](const mpfshost::log::Record& record)
                {
                    std::lock_guard lock {_mutex};
                    _records.push_back(Entry {record.level, std::string {record.category}, record.message});
                });
        }

        ~LogCapture()
        {
            mpfshost::log::setStderrSink();
            mpfshost::log::setLevel(_previousLevel);
        }

        LogCapture(const LogCapture&)            = delete;
        LogCapture& operator=(const LogCapture&) = delete;

        [[nodiscard]] bool waitFor(const std::string& categoryPrefix, const std::string& fragment)
        {
            for (int attempt = 0; attempt < 400; ++attempt)
            {
                {
                    std::lock_guard lock {_mutex};
                    for (const auto& entry : _records)
                    {
                        if (entry.category.starts_with(categoryPrefix)
                            && entry.message.find(fragment) != std::string::npos)
                        {
                            return true;
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return false;
        }

    private:
        struct Entry
        {
            mpfshost::log::Level level;
            std::string          category;
            std::string          message;
        };

        mpfshost::log::Level _previousLevel;
        std::mutex           _mutex;
        std::vector<Entry>   _records;
    };
} // namespace

TEST_CASE("server answers list-targets from the target source", "[server]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        { "vm-beta", 222,  fixture.root() / "vm-beta.sock"},
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"},
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::ListTargetsRequest, 5);

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));

    REQUIRE(type == mpfshost::MessageType::ListTargetsResponse);
    REQUIRE(requestId == 5);

    const auto response = mpfshost::decodeListTargetsResponse(payload);
    REQUIRE(response.has_value());
    REQUIRE(response->targets.size() == 2);
    REQUIRE(response->targets[0].name == "vm-beta");
    REQUIRE(response->targets[0].qemuPid == 222);
    REQUIRE(response->targets[0].qmpSocketPath == (fixture.root() / "vm-beta.sock").string());
    REQUIRE(response->targets[1].name == "vm-alpha");
}

TEST_CASE("server refuses a peer that the access policy does not allow", "[server][access]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::ServerOptions options {fixture.socketPath()};
    options.access.allowUid(static_cast<std::uint32_t>(::getuid()) + 1);
    RunningServer server {options, targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::ListTargetsRequest, 42);

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    REQUIRE(requestId == 42);

    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::AccessDenied);

    REQUIRE_FALSE(client.receive(type, requestId, payload));
    REQUIRE(backend.acquireCount() == 0);
}

TEST_CASE("server serves a peer that the access policy allows", "[server][access]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::ServerOptions options {fixture.socketPath()};
    options.access.allowUid(static_cast<std::uint32_t>(::getuid()));
    RunningServer server {options, targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::ListTargetsRequest, 7);

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ListTargetsResponse);
    REQUIRE(requestId == 7);
}

TEST_CASE("server admits a peer whose primary gid is allow-listed", "[server][access]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::ServerOptions options {fixture.socketPath()};
    options.access.allowGid(static_cast<std::uint32_t>(::getgid()));
    RunningServer server {options, targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::ListTargetsRequest, 9);

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ListTargetsResponse);
    REQUIRE(requestId == 9);
}

TEST_CASE("server admits a peer through its supplementary group list", "[server][access]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    const auto groups = mpfshost::readProcessGroupIds(::getpid());
    REQUIRE_FALSE(groups.empty());
    const auto primary       = static_cast<std::uint32_t>(::getgid());
    const auto supplementary = std::find_if(groups.begin(),
                                            groups.end(),
                                            [primary](std::uint32_t gid)
                                            {
                                                return gid != primary;
                                            });
    const auto allowed       = supplementary != groups.end() ? *supplementary : groups.front();

    mpfshost::ServerOptions options {fixture.socketPath()};
    options.access.allowGid(allowed);
    RunningServer server {options, targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::ListTargetsRequest, 11);

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ListTargetsResponse);
    REQUIRE(requestId == 11);
}

TEST_CASE("server closes a refused peer that sends a malformed frame without replying", "[server][access]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;

    mpfshost::ServerOptions options {fixture.socketPath()};
    options.access.allowUid(static_cast<std::uint32_t>(::getuid()) + 1);
    RunningServer server {options, targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.sendRaw(rawFrame(0xDEADBEEF, mpfshost::kProtocolVersion, 0x0001, 3, 0));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE_FALSE(client.receive(type, requestId, payload));
}

TEST_CASE("server creates the socket with the configured mode", "[server][access]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;

    const auto mode = [](const std::filesystem::path& path)
    {
        struct stat status {};
        REQUIRE(::stat(path.c_str(), &status) == 0);
        return status.st_mode & 0777;
    };

    SECTION("an open policy leaves the socket world accessible")
    {
        RunningServer server {fixture.socketPath(), targets, backend};
        ClientSocket  client {fixture.socketPath()};
        REQUIRE(mode(fixture.socketPath()) == 0666);
    }

    SECTION("an enforcing policy uses the configured mode")
    {
        mpfshost::ServerOptions options {fixture.socketPath()};
        options.access.allowUid(static_cast<std::uint32_t>(::getuid()));
        options.socketMode = 0660;
        RunningServer server {options, targets, backend};
        ClientSocket  client {fixture.socketPath()};
        REQUIRE(mode(fixture.socketPath()) == 0660);
    }
}

TEST_CASE("server logs the listening, connect, bind and disconnect lifecycle", "[server][log]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    LogCapture capture;

    {
        RunningServer server {fixture.socketPath(), targets, backend};
        REQUIRE(capture.waitFor("server", "listening on"));

        {
            ClientSocket client {fixture.socketPath()};
            REQUIRE(capture.waitFor("server", "client connected uid="));
            bindClient(client, 1, "vm-alpha");
            REQUIRE(capture.waitFor("session", "request BindRequest id=1"));
            REQUIRE(capture.waitFor("session", "bound to target vm-alpha"));
        }

        REQUIRE(capture.waitFor("server", "client disconnected conn="));
    }
}

TEST_CASE("server logs protocol failures as warnings", "[server][log]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;

    LogCapture capture;

    {
        RunningServer server {fixture.socketPath(), targets, backend};
        ClientSocket  client {fixture.socketPath()};

        client.send(mpfshost::MessageType::ListTargetsRequest, 3, std::vector<std::byte> {std::byte {0x01}});

        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(type == mpfshost::MessageType::ErrorResponse);

        REQUIRE(capture.waitFor("session", "error response code=MalformedPayload"));
    }
}

TEST_CASE("server answers list-targets with an empty list when no vm is running", "[server]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    RunningServer                       server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::ListTargetsRequest, 1);

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));

    REQUIRE(type == mpfshost::MessageType::ListTargetsResponse);
    const auto response = mpfshost::decodeListTargetsResponse(payload);
    REQUIRE(response.has_value());
    REQUIRE(response->targets.empty());
}

TEST_CASE("server answers bind for an unknown target with TargetNotFound", "[server]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    RunningServer                       server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::BindRequest, 6, mpfshost::encodeBindRequest({"ghost"}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));

    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    REQUIRE(requestId == 6);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::TargetNotFound);
    REQUIRE(backend.acquireCount() == 0);
}

TEST_CASE("server binds a known target and reports the session", "[server]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::BindRequest, 7, mpfshost::encodeBindRequest({"vm-alpha"}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));

    REQUIRE(type == mpfshost::MessageType::BindResponse);
    REQUIRE(requestId == 7);
    const auto response = mpfshost::decodeBindResponse(payload);
    REQUIRE(response.has_value());
    REQUIRE(response->sessionId != 0);
    REQUIRE(response->target.name == "vm-alpha");
    REQUIRE(response->target.qemuPid == 111);
    REQUIRE(backend.acquireCount() == 1);
}

TEST_CASE("server rejects a second bind on the same connection", "[server]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::BindRequest, 1, mpfshost::encodeBindRequest({"vm-alpha"}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::BindResponse);

    client.send(mpfshost::MessageType::BindRequest, 2, mpfshost::encodeBindRequest({"vm-alpha"}));
    REQUIRE(client.receive(type, requestId, payload));

    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    REQUIRE(requestId == 2);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::SessionAlreadyBound);
    REQUIRE(backend.acquireCount() == 1);
}

TEST_CASE("two concurrent clients bind to independent sessions", "[server]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"},
        { "vm-beta", 222,  fixture.root() / "vm-beta.sock"},
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket clientA {fixture.socketPath()};
    ClientSocket clientB {fixture.socketPath()};

    clientA.send(mpfshost::MessageType::BindRequest, 1, mpfshost::encodeBindRequest({"vm-alpha"}));
    clientB.send(mpfshost::MessageType::BindRequest, 2, mpfshost::encodeBindRequest({"vm-beta"}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;

    REQUIRE(clientA.receive(type, requestId, payload));
    const auto responseA = mpfshost::decodeBindResponse(payload);
    REQUIRE(responseA.has_value());
    REQUIRE(responseA->target.name == "vm-alpha");

    REQUIRE(clientB.receive(type, requestId, payload));
    const auto responseB = mpfshost::decodeBindResponse(payload);
    REQUIRE(responseB.has_value());
    REQUIRE(responseB->target.name == "vm-beta");

    REQUIRE(responseA->sessionId != responseB->sessionId);
    REQUIRE(backend.acquireCount() == 2);
    REQUIRE(backend.liveHandleCount() == 2);
}

TEST_CASE("server releases the backend handle when the client disconnects", "[server]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    {
        ClientSocket client {fixture.socketPath()};
        client.send(mpfshost::MessageType::BindRequest, 1, mpfshost::encodeBindRequest({"vm-alpha"}));

        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(type == mpfshost::MessageType::BindResponse);
        REQUIRE(backend.liveHandleCount() == 1);
    }

    REQUIRE(waitFor(
        [&backend]
        {
            return backend.liveHandleCount() == 0;
        }));
    REQUIRE(backend.acquireCount() == 1);
}

TEST_CASE("server reports TargetUnavailable and keeps serving when the backend fails", "[server]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failWith("device is not ready");
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::BindRequest, 1, mpfshost::encodeBindRequest({"vm-alpha"}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));

    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::TargetUnavailable);

    client.send(mpfshost::MessageType::ListTargetsRequest, 2);
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ListTargetsResponse);
}

TEST_CASE("server answers an unknown message type and keeps the connection alive", "[server]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    RunningServer                       server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(static_cast<mpfshost::MessageType>(0x7777), 3);

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));

    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    REQUIRE(requestId == 3);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::UnknownMessage);

    client.send(mpfshost::MessageType::ListTargetsRequest, 4);
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ListTargetsResponse);
}

TEST_CASE("server closes only the connection that sends a garbage frame", "[server]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    RunningServer                       server {fixture.socketPath(), targets, backend};

    ClientSocket garbage {fixture.socketPath()};
    ClientSocket healthy {fixture.socketPath()};

    const std::vector<std::byte> bytes(20, std::byte {0xAB});
    garbage.sendRaw(bytes);

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE_FALSE(garbage.receive(type, requestId, payload));

    healthy.send(mpfshost::MessageType::ListTargetsRequest, 9);
    REQUIRE(healthy.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ListTargetsResponse);
}

TEST_CASE("server rejects an unsupported protocol version and closes that connection", "[server]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    RunningServer                       server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.sendRaw(rawFrame(mpfshost::kProtocolMagic,
                            mpfshost::kProtocolVersion + 1,
                            static_cast<std::uint16_t>(mpfshost::MessageType::ListTargetsRequest),
                            12,
                            0));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));

    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    REQUIRE(requestId == 12);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::UnsupportedVersion);

    REQUIRE_FALSE(client.receive(type, requestId, payload));
}

TEST_CASE("server rejects a process list request before bind and keeps the connection alive", "[server][process]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    RunningServer                       server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::ProcessListRequest, 21, mpfshost::encodeProcessListRequest({}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    REQUIRE(requestId == 21);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::NotBound);

    client.send(mpfshost::MessageType::ListTargetsRequest, 22);
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ListTargetsResponse);
}

TEST_CASE("server rejects a malformed process list request", "[server][process]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::BindRequest, 1, mpfshost::encodeBindRequest({"vm-alpha"}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::BindResponse);

    mpfshost::ByteWriter truncated;
    truncated.writeU32(0);
    client.send(mpfshost::MessageType::ProcessListRequest, 2, truncated.bytes());
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::MalformedPayload);
}

TEST_CASE("server reports CapabilityUnavailable when the bound handle cannot list processes", "[server][process]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setHandlesBare(true);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::BindRequest, 1, mpfshost::encodeBindRequest({"vm-alpha"}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::BindResponse);

    client.send(mpfshost::MessageType::ProcessListRequest, 2, mpfshost::encodeProcessListRequest({}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::CapabilityUnavailable);
}

TEST_CASE("server forwards the process filter and returns the provider's matches", "[server][process]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setProcesses({
        {  4,   0,       "System", false},
        {100,   4, "explorer.exe", false},
        {200, 100,  "notepad.exe",  true},
    });
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::BindRequest, 1, mpfshost::encodeBindRequest({"vm-alpha"}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::BindResponse);

    client.send(mpfshost::MessageType::ProcessListRequest, 2, mpfshost::encodeProcessListRequest({0, "EXPL"}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ProcessListResponse);
    REQUIRE(requestId == 2);

    const auto response = mpfshost::decodeProcessListResponse(payload);
    REQUIRE(response.has_value());
    REQUIRE(response->processes.size() == 1);
    REQUIRE(response->processes[0].pid == 100);
    REQUIRE(response->processes[0].parentPid == 4);
    REQUIRE(response->processes[0].name == "explorer.exe");
    REQUIRE_FALSE(response->processes[0].wow64);

    const auto filter = backend.lastProcessFilter();
    REQUIRE(filter.has_value());
    REQUIRE(filter->pid == 0);
    REQUIRE(filter->nameFilter == "EXPL");
}

TEST_CASE("server reports Internal when the process provider fails", "[server][process]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failProcessesWith("paging is unavailable");
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::BindRequest, 1, mpfshost::encodeBindRequest({"vm-alpha"}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::BindResponse);

    client.send(mpfshost::MessageType::ProcessListRequest, 2, mpfshost::encodeProcessListRequest({}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::Internal);
    REQUIRE(error->message == "paging is unavailable");
}

TEST_CASE("server reports NotBound for a process info request before bind", "[server][process]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    RunningServer                       server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::ProcessInfoRequest, 31, mpfshost::encodeProcessInfoRequest({4, {}}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::NotBound);
}

TEST_CASE("server reports CapabilityUnavailable when the handle cannot describe processes", "[server][process]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setHandlesBare(true);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::ProcessInfoRequest, 2, mpfshost::encodeProcessInfoRequest({4, {}}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::CapabilityUnavailable);
}

TEST_CASE("server returns the process info the provider builds", "[server][process]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });

    mpfshost::ProcessInfoResponse expected;
    expected.process        = {4321, 4, "notepad.exe", false};
    expected.state          = 3;
    expected.sessionId      = 1;
    expected.integrityLevel = 3;
    expected.sid            = "S-1-5-18";
    expected.imagePath      = "C:\\Windows\\notepad.exe";
    expected.cmdline        = "notepad.exe a.txt";

    mpfshost::testing::FakeBackend backend;
    backend.setProcessInfo(expected);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::ProcessInfoRequest, 2, mpfshost::encodeProcessInfoRequest({0, "notepad.exe"}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ProcessInfoResponse);
    REQUIRE(requestId == 2);

    const auto response = mpfshost::decodeProcessInfoResponse(payload);
    REQUIRE(response.has_value());
    REQUIRE(response->process.pid == 4321);
    REQUIRE(response->process.name == "notepad.exe");
    REQUIRE(response->state == 3);
    REQUIRE(response->sessionId == 1);
    REQUIRE(response->integrityLevel == 3);
    REQUIRE(response->sid == "S-1-5-18");
    REQUIRE(response->imagePath == "C:\\Windows\\notepad.exe");
    REQUIRE(response->cmdline == "notepad.exe a.txt");

    const auto forwarded = backend.processInfoState()->lastRequest;
    REQUIRE(forwarded.has_value());
    REQUIRE(forwarded->pid == 0);
    REQUIRE(forwarded->name == "notepad.exe");
}

TEST_CASE("server maps a missing process to NotFound", "[server][process]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failProcessInfoWith("no process named 'ghost.exe'", mpfshost::LookupStatus::NotFound);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::ProcessInfoRequest, 2, mpfshost::encodeProcessInfoRequest({0, "ghost.exe"}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::NotFound);
    REQUIRE(error->message == "no process named 'ghost.exe'");
}

TEST_CASE("server reports Internal when the process info provider fails", "[server][process]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failProcessInfoWith("paging is unavailable");
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::ProcessInfoRequest, 2, mpfshost::encodeProcessInfoRequest({4, {}}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::Internal);
    REQUIRE(error->message == "paging is unavailable");
}

TEST_CASE("server rejects a malformed process info request", "[server][process]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    mpfshost::ByteWriter truncated;
    truncated.writeU32(4);
    client.send(mpfshost::MessageType::ProcessInfoRequest, 2, truncated.bytes());

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::MalformedPayload);
}

TEST_CASE("server reports ResultTooLarge and keeps the connection alive for an oversized list", "[server][process]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    std::vector<mpfshost::ProcessDescriptor> many;
    mpfshost::ProcessDescriptor              process;
    process.name = std::string(600, 'a');
    for (std::uint32_t index = 1; index <= 2000; ++index)
    {
        process.pid = index;
        many.push_back(process);
    }
    backend.setProcesses(std::move(many));
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::BindRequest, 1, mpfshost::encodeBindRequest({"vm-alpha"}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::BindResponse);

    client.send(mpfshost::MessageType::ProcessListRequest, 2, mpfshost::encodeProcessListRequest({}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::ResultTooLarge);

    client.send(mpfshost::MessageType::ListTargetsRequest, 3);
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ListTargetsResponse);
}

TEST_CASE("server streams a read spanning several frames and terminates it with an end frame", "[server][memory]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    const std::uint32_t size = mpfshost::kMaxPayloadLength + 1;
    client.send(mpfshost::MessageType::MemoryReadRequest, 2, mpfshost::encodeMemoryReadRequest({42, 0x1000, size}));

    std::vector<std::byte>   reassembled;
    std::vector<std::size_t> frameSizes;
    mpfshost::MemoryReadEnd  end;
    while (true)
    {
        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(requestId == 2);

        if (type == mpfshost::MessageType::MemoryReadEnd)
        {
            const auto decoded = mpfshost::decodeMemoryReadEnd(payload);
            REQUIRE(decoded.has_value());
            end = *decoded;
            break;
        }

        REQUIRE(type == mpfshost::MessageType::MemoryReadData);
        frameSizes.push_back(payload.size());
        reassembled.insert(reassembled.end(), payload.begin(), payload.end());
    }

    REQUIRE(frameSizes.size() == 2);
    REQUIRE(frameSizes[0] == mpfshost::kMaxPayloadLength);
    REQUIRE(frameSizes[1] == 1);
    REQUIRE(end.status == 0);
    REQUIRE(end.bytesDelivered == size);
    REQUIRE(end.error.empty());
    REQUIRE(reassembled == addressPattern(0x1000, size));

    const auto state = backend.memoryState();
    REQUIRE(state->lastReadPid == 42);
    REQUIRE(state->lastReadAddress == 0x1000);
    REQUIRE(state->lastReadSize == size);
}

TEST_CASE("a read of exactly one payload uses a single data frame", "[server][memory]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::MemoryReadRequest,
                2,
                mpfshost::encodeMemoryReadRequest({7, 0x2000, mpfshost::kMaxPayloadLength}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::MemoryReadData);
    REQUIRE(payload.size() == mpfshost::kMaxPayloadLength);

    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::MemoryReadEnd);
    const auto end = mpfshost::decodeMemoryReadEnd(payload);
    REQUIRE(end.has_value());
    REQUIRE(end->status == 0);
    REQUIRE(end->bytesDelivered == mpfshost::kMaxPayloadLength);
}

TEST_CASE("server rejects invalid memory ranges", "[server][memory]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    const std::vector<mpfshost::MemoryReadRequest> invalid {
        {7,         0x1000,                          0},
        {7,         0x1000, mpfshost::kMaxReadSize + 1},
        {7, UINT64_MAX - 1,                          4},
    };
    std::uint64_t requestId = 2;
    for (const auto& request : invalid)
    {
        client.send(mpfshost::MessageType::MemoryReadRequest, requestId, mpfshost::encodeMemoryReadRequest(request));

        mpfshost::MessageType  type;
        std::uint64_t          responseId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, responseId, payload));
        REQUIRE(type == mpfshost::MessageType::ErrorResponse);
        const auto error = mpfshost::decodeErrorResponse(payload);
        REQUIRE(error.has_value());
        REQUIRE(error->code == mpfshost::ErrorCode::InvalidRange);
        ++requestId;
    }
}

TEST_CASE("server reports NotBound for memory access on an unbound session", "[server][memory]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    RunningServer                       server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::MemoryReadRequest, 1, mpfshost::encodeMemoryReadRequest({7, 0x1000, 16}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::NotBound);
}

TEST_CASE("server reports CapabilityUnavailable when the handle cannot touch memory", "[server][memory]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setHandlesBare(true);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::MemoryReadRequest, 2, mpfshost::encodeMemoryReadRequest({7, 0x1000, 16}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::CapabilityUnavailable);
}

TEST_CASE("a failing read still closes the stream with a failed end frame", "[server][memory]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failMemoryWith("page is not present");
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::MemoryReadRequest, 2, mpfshost::encodeMemoryReadRequest({7, 0x1000, 16}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::MemoryReadEnd);
    const auto end = mpfshost::decodeMemoryReadEnd(payload);
    REQUIRE(end.has_value());
    REQUIRE(end->status == 1);
    REQUIRE(end->bytesDelivered == 0);
    REQUIRE(end->error == "page is not present");
}

TEST_CASE("a short read is reported as a failure instead of padded data", "[server][memory]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setShortRead(true);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::MemoryReadRequest, 2, mpfshost::encodeMemoryReadRequest({7, 0x1000, 16}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::MemoryReadEnd);
    const auto end = mpfshost::decodeMemoryReadEnd(payload);
    REQUIRE(end.has_value());
    REQUIRE(end->status == 1);
    REQUIRE(end->bytesDelivered == 0);
    REQUIRE(end->error.find("15") != std::string::npos);
}

TEST_CASE("server writes memory and reports the written byte count", "[server][memory]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    const std::vector<std::byte> data {std::byte {0xDE}, std::byte {0xAD}, std::byte {0xBE}, std::byte {0xEF}};
    client.send(mpfshost::MessageType::MemoryWriteRequest, 2, mpfshost::encodeMemoryWriteRequest({9, 0x3000, data}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::MemoryWriteResponse);
    REQUIRE(requestId == 2);
    const auto response = mpfshost::decodeMemoryWriteResponse(payload);
    REQUIRE(response.has_value());
    REQUIRE(response->bytesWritten == data.size());

    const auto state = backend.memoryState();
    REQUIRE(state->lastWritePid == 9);
    REQUIRE(state->lastWriteAddress == 0x3000);
    REQUIRE(state->lastWriteData == data);
}

TEST_CASE("server rejects an empty write", "[server][memory]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::MemoryWriteRequest, 2, mpfshost::encodeMemoryWriteRequest({9, 0x3000, {}}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::InvalidRange);
}

TEST_CASE("server reports Internal when the write provider fails", "[server][memory]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failWritesWith("write-protected");
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    const std::vector<std::byte> data {std::byte {0x01}};
    client.send(mpfshost::MessageType::MemoryWriteRequest, 2, mpfshost::encodeMemoryWriteRequest({9, 0x3000, data}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::Internal);
    REQUIRE(error->message == "write-protected");
}

TEST_CASE("server streams modules and terminates them with an end frame", "[server][module]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setModules({
        {0x7FF700000000ull, 0x7FF700001000ull, 0x20000, 0, 1, 0, 0, 0, false,    "ntdll.dll",              "", "", "", ""},
        {      0x401000ull,       0x401500ull, 0x10000, 0, 0, 0, 0, 0,  true,    "probe.exe", "C:\\probe.exe", "", "", ""},
        {      0x500000ull,       0x500100ull,  0x2000, 0, 0, 0, 0, 0, false, "kernel32.dll",              "", "", "", ""},
    });
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::ModuleListRequest, 2, mpfshost::encodeModuleListRequest({0, 0, ""}));

    std::vector<mpfshost::ModuleDescriptor> delivered;
    std::uint64_t                           endCount   = 0;
    int                                     dataFrames = 0;
    while (true)
    {
        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(requestId == 2);

        if (type == mpfshost::MessageType::ModuleListEnd)
        {
            const auto end = mpfshost::decodeModuleListEnd(payload);
            REQUIRE(end.has_value());
            REQUIRE(end->status == 0);
            REQUIRE(end->error.empty());
            endCount = end->moduleCount;
            break;
        }

        REQUIRE(type == mpfshost::MessageType::ModuleListData);
        const auto data = mpfshost::decodeModuleListData(payload);
        REQUIRE(data.has_value());
        ++dataFrames;
        delivered.insert(delivered.end(), data->modules.begin(), data->modules.end());
    }

    REQUIRE(dataFrames == 1);
    REQUIRE(endCount == 3);
    REQUIRE(delivered.size() == 3);
    REQUIRE(delivered[0].name == "ntdll.dll");
    REQUIRE(delivered[1].name == "probe.exe");
    REQUIRE(delivered[1].fullName == "C:\\probe.exe");
    REQUIRE(delivered[1].wow64);
    REQUIRE(delivered[2].name == "kernel32.dll");
}

TEST_CASE("server splits a large module list into ordered batches", "[server][module]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    std::vector<mpfshost::ModuleDescriptor> modules;
    for (std::uint64_t index = 0; index < 300; ++index)
    {
        mpfshost::ModuleDescriptor module;
        module.base = index;
        module.name = std::string(4096, 'a');
        modules.push_back(std::move(module));
    }
    backend.setModules(std::move(modules));
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::ModuleListRequest, 2, mpfshost::encodeModuleListRequest({0, 0, ""}));

    std::uint64_t endCount   = 0;
    int           dataFrames = 0;
    std::uint64_t expected   = 0;
    while (true)
    {
        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(requestId == 2);

        if (type == mpfshost::MessageType::ModuleListEnd)
        {
            const auto end = mpfshost::decodeModuleListEnd(payload);
            REQUIRE(end.has_value());
            REQUIRE(end->status == 0);
            endCount = end->moduleCount;
            break;
        }

        REQUIRE(type == mpfshost::MessageType::ModuleListData);
        const auto data = mpfshost::decodeModuleListData(payload);
        REQUIRE(data.has_value());
        REQUIRE(data->modules.size() > 0);
        REQUIRE(payload.size() <= mpfshost::kMaxPayloadLength);
        ++dataFrames;
        for (const auto& module : data->modules)
        {
            REQUIRE(module.base == expected);
            ++expected;
        }
    }

    REQUIRE(dataFrames > 1);
    REQUIRE(endCount == 300);
    REQUIRE(expected == 300);
}

TEST_CASE("server forwards module flags and name filter", "[server][module]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setModules({
        {0x401000ull, 0x401500ull, 0x10000, 0, 0, 0, 0, 0, false,    "probe.exe", "", "", "", ""},
        {0x500000ull, 0x500100ull,  0x2000, 0, 0, 0, 0, 0, false, "kernel32.dll", "", "", "", ""},
    });
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::ModuleListRequest,
                2,
                mpfshost::encodeModuleListRequest({0, mpfshost::kModuleFlagDebugInfo, "PROBE"}));

    std::uint64_t endCount = 0;
    int           names    = 0;
    while (true)
    {
        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));

        if (type == mpfshost::MessageType::ModuleListEnd)
        {
            const auto end = mpfshost::decodeModuleListEnd(payload);
            REQUIRE(end.has_value());
            endCount = end->moduleCount;
            break;
        }

        REQUIRE(type == mpfshost::MessageType::ModuleListData);
        const auto data = mpfshost::decodeModuleListData(payload);
        REQUIRE(data.has_value());
        names += static_cast<int>(data->modules.size());
        REQUIRE(data->modules[0].name == "probe.exe");
    }

    REQUIRE(endCount == 1);
    REQUIRE(names == 1);

    const auto last = backend.moduleState()->lastRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->flags == mpfshost::kModuleFlagDebugInfo);
    REQUIRE(last->nameFilter == "PROBE");
}

TEST_CASE("an empty module list delivers only the end frame", "[server][module]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::ModuleListRequest, 2, mpfshost::encodeModuleListRequest({0, 0, ""}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ModuleListEnd);
    const auto end = mpfshost::decodeModuleListEnd(payload);
    REQUIRE(end.has_value());
    REQUIRE(end->status == 0);
    REQUIRE(end->moduleCount == 0);
}

TEST_CASE("server rejects an unknown module flag", "[server][module]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    mpfshost::ByteWriter malformed;
    malformed.writeU32(0);
    malformed.writeU16(0x8000);
    malformed.writeString("");
    client.send(mpfshost::MessageType::ModuleListRequest, 2, malformed.bytes());

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::MalformedPayload);
}

TEST_CASE("server reports NotBound for a module list before bind", "[server][module]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    RunningServer                       server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::ModuleListRequest, 1, mpfshost::encodeModuleListRequest({0, 0, ""}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::NotBound);
}

TEST_CASE("server reports CapabilityUnavailable when the handle cannot list modules", "[server][module]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setHandlesBare(true);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::ModuleListRequest, 2, mpfshost::encodeModuleListRequest({0, 0, ""}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::CapabilityUnavailable);
}

TEST_CASE("server reports Internal when the module provider fails", "[server][module]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failModulesWith("module map unavailable");
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::ModuleListRequest, 2, mpfshost::encodeModuleListRequest({0, 0, ""}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::Internal);
    REQUIRE(error->message == "module map unavailable");
}

TEST_CASE("server streams memory map regions and forwards identifyModules", "[server][map]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setRegions({
        {0x10000,
         0x1FFFF, 0x04,
         0x1000, 1,
         static_cast<std::uint16_t>(mpfshost::kMemoryMapFlagImage | mpfshost::kMemoryMapFlagFile),
         0, "probe.exe"                                                                                                                  },
        {0x20000, 0x2FFFF, 0x02, 0x2000, 2,                                               mpfshost::kMemoryMapFlagPrivate, 3,      "heap"},
    });
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::MemoryMapRequest, 2, mpfshost::encodeMemoryMapRequest({77, true}));

    std::vector<mpfshost::MemoryMapRegion> delivered;
    std::uint64_t                          endCount   = 0;
    int                                    dataFrames = 0;
    while (true)
    {
        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(requestId == 2);

        if (type == mpfshost::MessageType::MemoryMapEnd)
        {
            const auto end = mpfshost::decodeMemoryMapEnd(payload);
            REQUIRE(end.has_value());
            REQUIRE(end->status == 0);
            endCount = end->regionCount;
            break;
        }

        REQUIRE(type == mpfshost::MessageType::MemoryMapData);
        const auto data = mpfshost::decodeMemoryMapData(payload);
        REQUIRE(data.has_value());
        ++dataFrames;
        delivered.insert(delivered.end(), data->regions.begin(), data->regions.end());
    }

    REQUIRE(dataFrames == 1);
    REQUIRE(endCount == 2);
    REQUIRE(delivered.size() == 2);
    REQUIRE(delivered[0].start == 0x10000);
    REQUIRE(delivered[0].end == 0x1FFFF);
    REQUIRE(delivered[0].description == "probe.exe");
    REQUIRE(delivered[1].heapNumber == 3);

    const auto last = backend.mapState()->lastRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->pid == 77);
    REQUIRE(last->identifyModules);
}

TEST_CASE("an empty memory map delivers only the end frame", "[server][map]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::MemoryMapRequest, 2, mpfshost::encodeMemoryMapRequest({77, false}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::MemoryMapEnd);
    const auto end = mpfshost::decodeMemoryMapEnd(payload);
    REQUIRE(end.has_value());
    REQUIRE(end->status == 0);
    REQUIRE(end->regionCount == 0);
}

TEST_CASE("server reports NotBound for a memory map before bind", "[server][map]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    RunningServer                       server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::MemoryMapRequest, 1, mpfshost::encodeMemoryMapRequest({77, false}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::NotBound);
}

TEST_CASE("server reports CapabilityUnavailable when the handle cannot list memory maps", "[server][map]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setHandlesBare(true);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::MemoryMapRequest, 2, mpfshost::encodeMemoryMapRequest({77, false}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::CapabilityUnavailable);
}

TEST_CASE("server reports Internal when the memory map provider fails", "[server][map]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failMapsWith("vad map unavailable");
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::MemoryMapRequest, 2, mpfshost::encodeMemoryMapRequest({77, false}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::Internal);
    REQUIRE(error->message == "vad map unavailable");
}

TEST_CASE("server streams per-page vad-ex entries", "[server][map]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::VadExDescriptor page;
    page.type      = 1;
    page.pml       = 4;
    page.flags     = mpfshost::kVadExFlagHardware;
    page.va        = 0x100000;
    page.pa        = 0x200000;
    page.pte       = 0x300000;
    page.protoType = 3;
    page.protoPa   = 0x400000;
    page.protoPte  = 0x500000;
    page.vaVadBase = 0x100000;
    backend.setVadExEntries({page});

    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::VadExListRequest, 2, mpfshost::encodeVadExListRequest({77, 16, 1}));

    std::vector<mpfshost::VadExDescriptor> delivered;
    std::uint64_t                          endCount = 0;
    while (true)
    {
        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(requestId == 2);

        if (type == mpfshost::MessageType::VadExListEnd)
        {
            const auto end = mpfshost::decodeVadExListEnd(payload);
            REQUIRE(end.has_value());
            REQUIRE(end->status == 0);
            endCount = end->entryCount;
            break;
        }

        REQUIRE(type == mpfshost::MessageType::VadExListData);
        const auto data = mpfshost::decodeVadExListData(payload);
        REQUIRE(data.has_value());
        delivered.insert(delivered.end(), data->entries.begin(), data->entries.end());
    }

    REQUIRE(endCount == 1);
    REQUIRE(delivered.size() == 1);
    REQUIRE(delivered[0].va == 0x100000);
    REQUIRE(delivered[0].pml == 4);
    REQUIRE(delivered[0].vaVadBase == 0x100000);

    const auto last = backend.vadExState()->lastRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->pid == 77);
    REQUIRE(last->pageOffset == 16);
    REQUIRE(last->pageCount == 1);
}

TEST_CASE("server rejects a vad-ex page count above the cap", "[server][map]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::VadExListRequest,
                2,
                mpfshost::encodeVadExListRequest({77, 0, mpfshost::kMaxPageCount + 1}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::ResultTooLarge);
}

TEST_CASE("server reports CapabilityUnavailable when the handle cannot list vad-ex pages", "[server][map]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setHandlesBare(true);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::VadExListRequest, 2, mpfshost::encodeVadExListRequest({77, 0, 1}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::CapabilityUnavailable);
}

TEST_CASE("server streams threads and terminates them with an end frame", "[server][thread]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::ThreadDescriptor running;
    running.tid                = 100;
    running.pid                = 4321;
    running.state              = 2;
    running.running            = true;
    running.affinity           = 3;
    running.userTime           = 250;
    running.kernelTime         = 125;
    running.instructionPointer = 0x401000;
    running.ethread            = 0xFFFF800000000001ull;

    mpfshost::ThreadDescriptor blocked;
    blocked.tid             = 101;
    blocked.pid             = 4321;
    blocked.state           = 5;
    blocked.suspendCount    = 1;
    blocked.callstackStatus = 1;

    mpfshost::ThreadDescriptor waiting;
    waiting.tid        = 102;
    waiting.pid        = 4321;
    waiting.waitReason = 7;

    backend.setThreads({running, blocked, waiting});
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::ThreadListRequest,
                2,
                mpfshost::encodeThreadListRequest({4321, 0, mpfshost::kThreadFlagCallstack}));

    std::vector<mpfshost::ThreadDescriptor> delivered;
    std::uint64_t                           endCount   = 0;
    int                                     dataFrames = 0;
    while (true)
    {
        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(requestId == 2);

        if (type == mpfshost::MessageType::ThreadListEnd)
        {
            const auto end = mpfshost::decodeThreadListEnd(payload);
            REQUIRE(end.has_value());
            REQUIRE(end->status == 0);
            REQUIRE(end->error.empty());
            endCount = end->threadCount;
            break;
        }

        REQUIRE(type == mpfshost::MessageType::ThreadListData);
        const auto data = mpfshost::decodeThreadListData(payload);
        REQUIRE(data.has_value());
        ++dataFrames;
        delivered.insert(delivered.end(), data->threads.begin(), data->threads.end());
    }

    REQUIRE(dataFrames == 1);
    REQUIRE(endCount == 3);
    REQUIRE(delivered.size() == 3);
    REQUIRE(delivered[0].tid == 100);
    REQUIRE(delivered[0].pid == 4321);
    REQUIRE(delivered[0].state == 2);
    REQUIRE(delivered[0].running);
    REQUIRE(delivered[0].affinity == 3);
    REQUIRE(delivered[0].userTime == 250);
    REQUIRE(delivered[0].kernelTime == 125);
    REQUIRE(delivered[0].instructionPointer == 0x401000);
    REQUIRE(delivered[0].ethread == 0xFFFF800000000001ull);
    REQUIRE(delivered[1].tid == 101);
    REQUIRE(delivered[1].state == 5);
    REQUIRE(delivered[1].suspendCount == 1);
    REQUIRE(delivered[1].callstackStatus == 1);
    REQUIRE(delivered[1].frames.empty());
    REQUIRE(delivered[2].tid == 102);
    REQUIRE(delivered[2].waitReason == 7);

    const auto last = backend.threadState()->lastRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->pid == 4321);
    REQUIRE(last->tid == 0);
    REQUIRE(last->flags == mpfshost::kThreadFlagCallstack);
}

TEST_CASE("server forwards a thread id filter and delivers one thread", "[server][thread]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::ThreadDescriptor first;
    first.tid = 100;
    first.pid = 4321;
    mpfshost::ThreadDescriptor second;
    second.tid = 101;
    second.pid = 4321;
    backend.setThreads({first, second});
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::ThreadListRequest, 2, mpfshost::encodeThreadListRequest({4321, 101, 0}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ThreadListData);
    const auto data = mpfshost::decodeThreadListData(payload);
    REQUIRE(data.has_value());
    REQUIRE(data->threads.size() == 1);
    REQUIRE(data->threads[0].tid == 101);

    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ThreadListEnd);
    const auto end = mpfshost::decodeThreadListEnd(payload);
    REQUIRE(end.has_value());
    REQUIRE(end->threadCount == 1);

    const auto last = backend.threadState()->lastRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->tid == 101);
}

TEST_CASE("server splits a large thread list into ordered batches", "[server][thread]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    std::vector<mpfshost::ThreadDescriptor> threads;
    for (std::uint32_t index = 0; index < 300; ++index)
    {
        mpfshost::ThreadDescriptor thread;
        thread.tid = index + 1;
        mpfshost::ThreadFrame frame;
        frame.function = std::string(4096, 'f');
        thread.frames.push_back(std::move(frame));
        threads.push_back(std::move(thread));
    }
    backend.setThreads(std::move(threads));
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::ThreadListRequest,
                2,
                mpfshost::encodeThreadListRequest({4321, 0, mpfshost::kThreadFlagCallstack}));

    std::uint64_t endCount   = 0;
    int           dataFrames = 0;
    std::uint32_t expected   = 1;
    while (true)
    {
        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(requestId == 2);

        if (type == mpfshost::MessageType::ThreadListEnd)
        {
            const auto end = mpfshost::decodeThreadListEnd(payload);
            REQUIRE(end.has_value());
            REQUIRE(end->status == 0);
            endCount = end->threadCount;
            break;
        }

        REQUIRE(type == mpfshost::MessageType::ThreadListData);
        const auto data = mpfshost::decodeThreadListData(payload);
        REQUIRE(data.has_value());
        REQUIRE(!data->threads.empty());
        REQUIRE(payload.size() <= mpfshost::kMaxPayloadLength);
        ++dataFrames;
        for (const auto& thread : data->threads)
        {
            REQUIRE(thread.tid == expected);
            ++expected;
        }
    }

    REQUIRE(dataFrames > 1);
    REQUIRE(endCount == 300);
    REQUIRE(expected == 301);
}

TEST_CASE("an empty thread list delivers only the end frame", "[server][thread]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    mpfshost::ThreadDescriptor     thread;
    thread.tid = 100;
    thread.pid = 4321;
    backend.setThreads({thread});
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::ThreadListRequest, 2, mpfshost::encodeThreadListRequest({4321, 999, 0}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ThreadListEnd);
    const auto end = mpfshost::decodeThreadListEnd(payload);
    REQUIRE(end.has_value());
    REQUIRE(end->status == 0);
    REQUIRE(end->threadCount == 0);
}

TEST_CASE("server rejects an unknown thread flag", "[server][thread]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    mpfshost::ByteWriter malformed;
    malformed.writeU32(4321);
    malformed.writeU32(0);
    malformed.writeU16(0x8000);
    client.send(mpfshost::MessageType::ThreadListRequest, 2, malformed.bytes());

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::MalformedPayload);
}

TEST_CASE("server rejects a truncated thread request and keeps serving", "[server][thread]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    mpfshost::ThreadDescriptor     thread;
    thread.tid = 100;
    thread.pid = 4321;
    backend.setThreads({thread});
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    mpfshost::ByteWriter truncated;
    truncated.writeU32(4321);
    truncated.writeU32(0);
    client.send(mpfshost::MessageType::ThreadListRequest, 2, truncated.bytes());

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::MalformedPayload);

    client.send(mpfshost::MessageType::ThreadListRequest, 3, mpfshost::encodeThreadListRequest({4321, 0, 0}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(requestId == 3);
    REQUIRE(type == mpfshost::MessageType::ThreadListData);
    const auto data = mpfshost::decodeThreadListData(payload);
    REQUIRE(data.has_value());
    REQUIRE(data->threads.size() == 1);
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ThreadListEnd);
}

TEST_CASE("server reports NotBound for a thread list before bind", "[server][thread]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    RunningServer                       server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::ThreadListRequest, 1, mpfshost::encodeThreadListRequest({4321, 0, 0}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::NotBound);
}

TEST_CASE("server reports CapabilityUnavailable when the handle cannot list threads", "[server][thread]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setHandlesBare(true);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::ThreadListRequest, 2, mpfshost::encodeThreadListRequest({4321, 0, 0}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::CapabilityUnavailable);
}

TEST_CASE("server reports Internal when the thread provider fails", "[server][thread]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failThreadsWith("thread map unavailable");
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::ThreadListRequest, 2, mpfshost::encodeThreadListRequest({4321, 0, 0}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::Internal);
    REQUIRE(error->message == "thread map unavailable");
}

TEST_CASE("server reports NotBound for a symbol lookup before bind", "[server][module]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    RunningServer                       server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(
        mpfshost::MessageType::SymbolLookupRequest, 41, mpfshost::encodeSymbolLookupRequest({4, "ntdll.dll", {}}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::NotBound);
}

TEST_CASE("server reports CapabilityUnavailable when the handle cannot resolve symbols", "[server][module]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setHandlesBare(true);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(
        mpfshost::MessageType::SymbolLookupRequest, 2, mpfshost::encodeSymbolLookupRequest({4, "ntdll.dll", {}}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::CapabilityUnavailable);
}

TEST_CASE("server returns the symbol lookup the provider builds", "[server][module]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setSymbolResponse({0x7ffb10000000ull, 0x7ffb10012340ull, 0x12340, true});
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::SymbolLookupRequest,
                2,
                mpfshost::encodeSymbolLookupRequest({4321, "kernel32.dll", "CreateFileW"}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::SymbolLookupResponse);

    const auto response = mpfshost::decodeSymbolLookupResponse(payload);
    REQUIRE(response.has_value());
    REQUIRE(response->moduleBase == 0x7ffb10000000ull);
    REQUIRE(response->address == 0x7ffb10012340ull);
    REQUIRE(response->offset == 0x12340);
    REQUIRE(response->found);

    const auto forwarded = backend.moduleLookupState()->lastSymbolRequest;
    REQUIRE(forwarded.has_value());
    REQUIRE(forwarded->pid == 4321);
    REQUIRE(forwarded->module == "kernel32.dll");
    REQUIRE(forwarded->symbol == "CreateFileW");
}

TEST_CASE("server maps an unknown module to NotFound", "[server][module]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failSymbolWith("module 'ghost.dll' is not loaded in process 4", mpfshost::LookupStatus::NotFound);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(
        mpfshost::MessageType::SymbolLookupRequest, 2, mpfshost::encodeSymbolLookupRequest({4, "ghost.dll", {}}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::NotFound);
    REQUIRE(error->message == "module 'ghost.dll' is not loaded in process 4");
}

TEST_CASE("server reports Internal when the symbol provider fails", "[server][module]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failSymbolWith("the module table is unavailable");
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(
        mpfshost::MessageType::SymbolLookupRequest, 2, mpfshost::encodeSymbolLookupRequest({4, "ntdll.dll", {}}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::Internal);
    REQUIRE(error->message == "the module table is unavailable");
}

TEST_CASE("server returns the pdb responses the provider builds", "[server][module]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setPdbLoadResponse({"nt"});
    backend.setPdbSymbolNameResponse({"PsGetCurrentProcess", 4});
    backend.setPdbSymbolAddressResponse({0xfffff80000123456ull});
    backend.setPdbTypeSizeResponse({0x4d0});
    backend.setPdbTypeChildOffsetResponse({0x440});
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;

    client.send(
        mpfshost::MessageType::PdbLoadRequest, 2, mpfshost::encodePdbLoadRequest({4321, 0xfffff80000000000ull}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::PdbLoadResponse);
    const auto load = mpfshost::decodePdbLoadResponse(payload);
    REQUIRE(load.has_value());
    REQUIRE(load->module == "nt");
    REQUIRE(backend.pdbState()->lastLoadRequest->pid == 4321);
    REQUIRE(backend.pdbState()->lastLoadRequest->moduleBase == 0xfffff80000000000ull);

    client.send(mpfshost::MessageType::PdbSymbolNameRequest,
                3,
                mpfshost::encodePdbSymbolNameRequest({"nt", 0xfffff80000123456ull}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::PdbSymbolNameResponse);
    const auto name = mpfshost::decodePdbSymbolNameResponse(payload);
    REQUIRE(name.has_value());
    REQUIRE(name->symbol == "PsGetCurrentProcess");
    REQUIRE(name->displacement == 4);
    REQUIRE(backend.pdbState()->lastNameRequest->module == "nt");
    REQUIRE(backend.pdbState()->lastNameRequest->address == 0xfffff80000123456ull);

    client.send(mpfshost::MessageType::PdbSymbolAddressRequest,
                4,
                mpfshost::encodePdbSymbolAddressRequest({"nt", "PsGetCurrentProcess"}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::PdbSymbolAddressResponse);
    const auto address = mpfshost::decodePdbSymbolAddressResponse(payload);
    REQUIRE(address.has_value());
    REQUIRE(address->address == 0xfffff80000123456ull);
    REQUIRE(backend.pdbState()->lastAddressRequest->module == "nt");
    REQUIRE(backend.pdbState()->lastAddressRequest->symbol == "PsGetCurrentProcess");

    client.send(mpfshost::MessageType::PdbTypeSizeRequest, 5, mpfshost::encodePdbTypeSizeRequest({"nt", "_EPROCESS"}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::PdbTypeSizeResponse);
    const auto typeSize = mpfshost::decodePdbTypeSizeResponse(payload);
    REQUIRE(typeSize.has_value());
    REQUIRE(typeSize->size == 0x4d0);
    REQUIRE(backend.pdbState()->lastTypeSizeRequest->module == "nt");
    REQUIRE(backend.pdbState()->lastTypeSizeRequest->type == "_EPROCESS");

    client.send(mpfshost::MessageType::PdbTypeChildOffsetRequest,
                6,
                mpfshost::encodePdbTypeChildOffsetRequest({"nt", "_EPROCESS", "UniqueProcessId"}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::PdbTypeChildOffsetResponse);
    const auto childOffset = mpfshost::decodePdbTypeChildOffsetResponse(payload);
    REQUIRE(childOffset.has_value());
    REQUIRE(childOffset->offset == 0x440);
    REQUIRE(backend.pdbState()->lastChildOffsetRequest->module == "nt");
    REQUIRE(backend.pdbState()->lastChildOffsetRequest->type == "_EPROCESS");
    REQUIRE(backend.pdbState()->lastChildOffsetRequest->child == "UniqueProcessId");
}

TEST_CASE("server reports CapabilityUnavailable when the handle cannot load symbols", "[server][module]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setHandlesBare(true);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::PdbLoadRequest, 2, mpfshost::encodePdbLoadRequest({4, 0xfffff80000000000ull}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::CapabilityUnavailable);
}

TEST_CASE("server maps a missing pdb to NotFound", "[server][module]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failPdbSymbolNameWith("no pdb symbol matches the address in module 'nt'", mpfshost::LookupStatus::NotFound);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::PdbSymbolNameRequest,
                2,
                mpfshost::encodePdbSymbolNameRequest({"nt", 0xfffff80000123456ull}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::NotFound);
}

TEST_CASE("server reports NotBound for a module layout before bind", "[server][module]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    RunningServer                       server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::ModuleLayoutRequest, 51, mpfshost::encodeModuleLayoutRequest({4, "ntdll.dll"}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::NotBound);
}

TEST_CASE("server reports CapabilityUnavailable when the handle cannot describe module layouts", "[server][module]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setHandlesBare(true);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::ModuleLayoutRequest, 2, mpfshost::encodeModuleLayoutRequest({4, "ntdll.dll"}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::CapabilityUnavailable);
}

TEST_CASE("server returns the module layout the provider builds", "[server][module]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });

    mpfshost::ModuleLayoutResponse expected;
    expected.moduleBase = 0x00007ff700000000ull;
    expected.sections.push_back({".text", expected.moduleBase + 0x1000, 0x2000, 0x400, 0x2000, 0x60000020});
    for (std::uint32_t i = 0; i < mpfshost::kModuleDataDirectoryCount; ++i)
    {
        expected.directories.push_back({i, i * 0x1000, i * 0x20});
    }

    mpfshost::testing::FakeBackend backend;
    backend.setLayoutResponse(expected);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(
        mpfshost::MessageType::ModuleLayoutRequest, 2, mpfshost::encodeModuleLayoutRequest({4321, "notepad.exe"}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ModuleLayoutResponse);

    const auto response = mpfshost::decodeModuleLayoutResponse(payload);
    REQUIRE(response.has_value());
    REQUIRE(response->moduleBase == 0x00007ff700000000ull);
    REQUIRE(response->sections.size() == 1);
    REQUIRE(response->sections[0].name == ".text");
    REQUIRE(response->directories.size() == mpfshost::kModuleDataDirectoryCount);

    const auto forwarded = backend.moduleLookupState()->lastLayoutRequest;
    REQUIRE(forwarded.has_value());
    REQUIRE(forwarded->pid == 4321);
    REQUIRE(forwarded->module == "notepad.exe");
}

TEST_CASE("server maps an unknown module layout to NotFound", "[server][module]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failLayoutWith("module 'ghost.dll' is not loaded in process 4", mpfshost::LookupStatus::NotFound);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::ModuleLayoutRequest, 2, mpfshost::encodeModuleLayoutRequest({4, "ghost.dll"}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::NotFound);
    REQUIRE(error->message == "module 'ghost.dll' is not loaded in process 4");
}

TEST_CASE("server reports Internal when the module layout provider fails", "[server][module]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failLayoutWith("section headers are unreadable");
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::ModuleLayoutRequest, 2, mpfshost::encodeModuleLayoutRequest({4, "ntdll.dll"}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::Internal);
    REQUIRE(error->message == "section headers are unreadable");
}

TEST_CASE("cached backend reuses one handle across two bound connections", "[server][cache]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });

    mpfshost::testing::CountingBackend inner;
    mpfshost::HandleCacheOptions       options;
    options.idleTimeout   = std::chrono::seconds {60};
    options.sweepInterval = std::chrono::milliseconds {0};
    mpfshost::CachingBackend backend {inner, options};
    RunningServer            server {fixture.socketPath(), targets, backend};

    {
        ClientSocket client {fixture.socketPath()};
        bindClient(client, 1, "vm-alpha");
        REQUIRE(inner.acquireCount() == 1);
        REQUIRE(inner.liveHandleCount() == 1);
    }

    // The session is gone, but the cache keeps the handle so the next connection does not pay for it again.
    REQUIRE(waitFor(
        [&inner]
        {
            return inner.liveHandleCount() == 1;
        }));

    {
        ClientSocket client {fixture.socketPath()};
        bindClient(client, 2, "vm-alpha");
    }

    REQUIRE(inner.acquireCount() == 1);
    REQUIRE(inner.liveHandleCount() == 1);
    REQUIRE(backend.size() == 1);
}

TEST_CASE("cached backend isolates targets and does not keep a failed bind", "[server][cache]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"},
        { "vm-beta", 222,  fixture.root() / "vm-beta.sock"},
        {"vm-gamma", 333, fixture.root() / "vm-gamma.sock"},
    });

    mpfshost::testing::CountingBackend inner;
    mpfshost::HandleCacheOptions       options;
    options.idleTimeout   = std::chrono::seconds {60};
    options.sweepInterval = std::chrono::milliseconds {0};
    mpfshost::CachingBackend backend {inner, options};
    RunningServer            server {fixture.socketPath(), targets, backend};

    {
        ClientSocket client {fixture.socketPath()};
        bindClient(client, 1, "vm-alpha");
    }
    {
        ClientSocket client {fixture.socketPath()};
        bindClient(client, 2, "vm-beta");
    }

    REQUIRE(inner.acquireCount() == 2);
    REQUIRE(inner.liveHandleCount() == 2);

    inner.failWith("VMMDLL_Initialize failed");

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    {
        ClientSocket client {fixture.socketPath()};
        client.send(mpfshost::MessageType::BindRequest, 3, mpfshost::encodeBindRequest({"vm-gamma"}));
        REQUIRE(client.receive(type, requestId, payload));
    }

    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    REQUIRE(requestId == 3);
    const auto failure = mpfshost::decodeErrorResponse(payload);
    REQUIRE(failure.has_value());
    REQUIRE(failure->code == mpfshost::ErrorCode::TargetUnavailable);
    REQUIRE(failure->message == "VMMDLL_Initialize failed");
    REQUIRE(inner.acquireCount() == 3);

    inner.clearFailure();
    {
        ClientSocket client {fixture.socketPath()};
        bindClient(client, 4, "vm-gamma");
    }

    // The failed attempt left nothing behind, so the retry creates the handle normally.
    REQUIRE(inner.acquireCount() == 4);
    REQUIRE(inner.liveHandleCount() == 3);
    REQUIRE(backend.size() == 3);
}

TEST_CASE("server closes a connection that stays idle beyond the idle timeout", "[server][timeout]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::ServerOptions options {fixture.socketPath()};
    options.idleTimeout = std::chrono::milliseconds {100};
    RunningServer server {options, targets, backend};

    LogCapture   logs;
    ClientSocket client {fixture.socketPath()};
    client.setReceiveTimeout(std::chrono::seconds {5});

    std::byte  byte {};
    const auto result = ::recv(client.fd(), &byte, 1, 0);
    CHECK(result == 0);
    CHECK(logs.waitFor("session", "idle timeout"));
}

TEST_CASE("server reports a request that exceeds the request deadline", "[server][timeout]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.memoryState()->readDelay = std::chrono::milliseconds {400};

    mpfshost::ServerOptions options {fixture.socketPath()};
    options.requestTimeout = std::chrono::milliseconds {100};
    RunningServer server {options, targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.setReceiveTimeout(std::chrono::seconds {5});
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::MemoryReadRequest, 7, mpfshost::encodeMemoryReadRequest({4, 0x1000, 16}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    REQUIRE(requestId == 7);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::Timeout);

    // The deadline expired before any frame went out, so the request was answered and the session survives.
    backend.memoryState()->readDelay = std::chrono::milliseconds::zero();
    client.send(mpfshost::MessageType::MemoryReadRequest, 8, mpfshost::encodeMemoryReadRequest({4, 0x1000, 16}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::MemoryReadData);
    REQUIRE(requestId == 8);
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::MemoryReadEnd);
}

TEST_CASE("server ends a session whose reader stops draining the stream", "[server][timeout]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::ServerOptions options {fixture.socketPath()};
    options.requestTimeout = std::chrono::milliseconds {200};
    RunningServer server {options, targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.setReceiveTimeout(std::chrono::seconds {10});
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::MemoryReadRequest, 9, mpfshost::encodeMemoryReadRequest({4, 0x1000, 4u << 20}));

    // Leaving the stream unread lets the socket buffer fill, so the host's bounded write must give up on its own.
    std::this_thread::sleep_for(std::chrono::milliseconds {500});
    const auto started = std::chrono::steady_clock::now();
    std::byte  buffer[64 * 1024];
    ssize_t    result = 1;
    while (result > 0)
    {
        result = ::recv(client.fd(), buffer, sizeof(buffer), 0);
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK(result == 0);
    CHECK(elapsed < std::chrono::seconds {5});
}

TEST_CASE("server serves from a bounded pool and refuses a full queue", "[server][pool]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.memoryState()->readDelay = std::chrono::milliseconds {1000};

    mpfshost::ServerOptions options {fixture.socketPath()};
    options.workers            = 1;
    options.pendingConnections = 1;
    RunningServer server {options, targets, backend};

    ClientSocket first {fixture.socketPath()};
    bindClient(first, 1, "vm-alpha");
    first.send(mpfshost::MessageType::MemoryReadRequest, 2, mpfshost::encodeMemoryReadRequest({4, 0x1000, 16}));
    REQUIRE(waitFor(
        [&backend]
        {
            return backend.memoryState()->lastReadSize == 16;
        }));

    ClientSocket second {fixture.socketPath()};
    REQUIRE(waitFor(
        [&server]
        {
            return server.pendingConnections() == 1;
        }));

    ClientSocket third {fixture.socketPath()};
    third.send(mpfshost::MessageType::ListTargetsRequest, 3);

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(third.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    REQUIRE(requestId == 3);
    const auto refusal = mpfshost::decodeErrorResponse(payload);
    REQUIRE(refusal.has_value());
    REQUIRE(refusal->code == mpfshost::ErrorCode::Busy);
    CHECK(server.pendingConnections() <= 1);

    REQUIRE(first.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::MemoryReadData);
    REQUIRE(requestId == 2);
    REQUIRE(first.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::MemoryReadEnd);
    first.close();

    // The queued second connection is served once the single worker finished the delayed request.
    second.send(mpfshost::MessageType::ListTargetsRequest, 4);
    REQUIRE(second.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ListTargetsResponse);
    REQUIRE(requestId == 4);

    second.close();
    third.close();
    REQUIRE(waitFor(
        [&server]
        {
            return server.activeSessions() == 0 && server.pendingConnections() == 0;
        }));
}

TEST_CASE("server keeps accepting while a refused client stays silent", "[server][pool]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.memoryState()->readDelay = std::chrono::milliseconds {300};

    mpfshost::ServerOptions options {fixture.socketPath()};
    options.workers            = 1;
    options.pendingConnections = 1;

    LogCapture    capture;
    RunningServer server {options, targets, backend};

    ClientSocket first {fixture.socketPath()};
    bindClient(first, 1, "vm-alpha");
    first.send(mpfshost::MessageType::MemoryReadRequest, 2, mpfshost::encodeMemoryReadRequest({4, 0x1000, 16}));
    REQUIRE(waitFor(
        [&backend]
        {
            return backend.memoryState()->lastReadSize == 16;
        }));

    ClientSocket second {fixture.socketPath()};
    REQUIRE(waitFor(
        [&server]
        {
            return server.pendingConnections() == 1;
        }));

    // The queue is full. This client never sends a frame, so its refusal waits out the past-frame timeout.
    ClientSocket silent {fixture.socketPath()};
    REQUIRE(capture.waitFor("server", "connection queue is full, refusing conn="));

    // Freeing the worker and the queue slot must be enough for the next client, even while the refusal is pending.
    first.close();
    second.close();
    REQUIRE(waitFor(
        [&server]
        {
            return server.activeSessions() == 0 && server.pendingConnections() == 0;
        }));

    const auto   started = std::chrono::steady_clock::now();
    ClientSocket next {fixture.socketPath()};
    next.send(mpfshost::MessageType::ListTargetsRequest, 9);

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(next.receive(type, requestId, payload));
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE(type == mpfshost::MessageType::ListTargetsResponse);
    REQUIRE(requestId == 9);
    CHECK(elapsed < std::chrono::milliseconds {100});
}

TEST_CASE("server serves sequential clients from a two-slot pool and leaves no residue", "[server][pool]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::ServerOptions options {fixture.socketPath()};
    options.workers = 2;
    RunningServer server {options, targets, backend};

    for (std::uint64_t attempt = 1; attempt <= 5; ++attempt)
    {
        ClientSocket client {fixture.socketPath()};
        client.send(mpfshost::MessageType::ListTargetsRequest, attempt);

        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(type == mpfshost::MessageType::ListTargetsResponse);
        REQUIRE(requestId == attempt);
    }

    REQUIRE(waitFor(
        [&server]
        {
            return server.activeSessions() == 0 && server.pendingConnections() == 0;
        }));
}

TEST_CASE("server shutdown returns while a delayed request is still in flight", "[server][pool]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.memoryState()->readDelay = std::chrono::milliseconds {1000};

    mpfshost::ServerOptions options {fixture.socketPath()};
    auto                    server = std::make_unique<mpfshost::Server>(options, targets, backend);
    std::jthread            runner {[&server]
                                    {
                             std::string error;
                             (void)server->run(error);
                                    }};

    {
        ClientSocket client {fixture.socketPath()};
        bindClient(client, 1, "vm-alpha");
        client.send(mpfshost::MessageType::MemoryReadRequest, 2, mpfshost::encodeMemoryReadRequest({4, 0x1000, 16}));
        REQUIRE(waitFor(
            [&backend]
            {
                return backend.memoryState()->lastReadSize == 16;
            }));

        const auto started = std::chrono::steady_clock::now();
        server->requestStop();
        runner.join();
        server.reset();
        const auto elapsed = std::chrono::steady_clock::now() - started;

        CHECK(elapsed < std::chrono::seconds {2});
    }
}

TEST_CASE("server cancels an in-flight request and acknowledges the cancel", "[server][cancel]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.memoryState()->readDelay = std::chrono::milliseconds {400};

    mpfshost::ServerOptions options {fixture.socketPath()};
    options.workers = 1;
    RunningServer server {options, targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.setReceiveTimeout(std::chrono::seconds {5});
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::MemoryReadRequest, 2, mpfshost::encodeMemoryReadRequest({4, 0x1000, 16}));
    client.send(mpfshost::MessageType::CancelRequest, 99, mpfshost::encodeCancelRequest({2}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    REQUIRE(requestId == 2);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::Cancelled);

    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::CancelResponse);
    REQUIRE(requestId == 99);
    const auto response = mpfshost::decodeCancelResponse(payload);
    REQUIRE(response.has_value());
    REQUIRE(response->cancelled == 1);
    REQUIRE(response->requestId == 2);

    // The aborted request was answered, so the session stays usable for the next request.
    client.send(mpfshost::MessageType::ListTargetsRequest, 3);
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ListTargetsResponse);
    REQUIRE(requestId == 3);
}

TEST_CASE("server declines a cancel when nothing is in flight", "[server][cancel]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.setReceiveTimeout(std::chrono::seconds {5});
    client.send(mpfshost::MessageType::CancelRequest, 5, mpfshost::encodeCancelRequest({42}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::CancelResponse);
    REQUIRE(requestId == 5);
    const auto response = mpfshost::decodeCancelResponse(payload);
    REQUIRE(response.has_value());
    REQUIRE(response->cancelled == 0);
    REQUIRE(response->requestId == 42);

    client.send(mpfshost::MessageType::ListTargetsRequest, 6);
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ListTargetsResponse);
}

TEST_CASE("server declines a cancel for a request that already answered", "[server][cancel]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.setReceiveTimeout(std::chrono::seconds {5});
    client.send(mpfshost::MessageType::ListTargetsRequest, 7);

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ListTargetsResponse);

    client.send(mpfshost::MessageType::CancelRequest, 8, mpfshost::encodeCancelRequest({7}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::CancelResponse);
    REQUIRE(requestId == 8);
    const auto response = mpfshost::decodeCancelResponse(payload);
    REQUIRE(response.has_value());
    REQUIRE(response->cancelled == 0);
}

TEST_CASE("server rejects a malformed cancel payload and keeps the session usable", "[server][cancel]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.setReceiveTimeout(std::chrono::seconds {5});
    client.send(mpfshost::MessageType::CancelRequest, 9, std::span<const std::byte> {});

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    REQUIRE(requestId == 9);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::MalformedPayload);

    client.send(mpfshost::MessageType::ListTargetsRequest, 10);
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ListTargetsResponse);
}

TEST_CASE("server ends the connection when a cancel arrives after frames were streamed", "[server][cancel]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.setReceiveTimeout(std::chrono::seconds {10});
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::MemoryReadRequest, 20, mpfshost::encodeMemoryReadRequest({4, 0x1000, 4u << 20}));
    REQUIRE(waitFor(
        [&backend]
        {
            return backend.memoryState()->lastReadSize == (4u << 20);
        }));
    // The host is blocked writing the first frame into a full socket, so the cancel lands after it started.
    std::this_thread::sleep_for(std::chrono::milliseconds {100});
    client.send(mpfshost::MessageType::CancelRequest, 21, mpfshost::encodeCancelRequest({20}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    bool                   sawData = false;
    bool                   sawEnd  = false;
    while (client.receive(type, requestId, payload))
    {
        if (type == mpfshost::MessageType::MemoryReadData)
        {
            sawData = true;
        }
        if (type == mpfshost::MessageType::MemoryReadEnd)
        {
            sawEnd = true;
        }
    }
    CHECK(sawData);
    CHECK_FALSE(sawEnd);
}

TEST_CASE("server returns the virtual region the provider reports", "[server][vquery]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });

    mpfshost::VirtualRegion region;
    region.baseAddress       = 0x00007ff700000000ull;
    region.allocationBase    = 0x00007ff700000000ull;
    region.regionSize        = 0x2000;
    region.allocationProtect = 0x80;
    region.state             = 0x1000;
    region.protect           = 0x20;
    region.type              = 0x1000000;

    mpfshost::testing::FakeBackend backend;
    backend.setVirtualRegion(region);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::VirtualQueryRequest,
                2,
                mpfshost::encodeVirtualQueryRequest({4321, 0x00007ff700001008ull}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::VirtualQueryResponse);

    const auto response = mpfshost::decodeVirtualQueryResponse(payload);
    REQUIRE(response.has_value());
    REQUIRE(response->region.baseAddress == region.baseAddress);
    REQUIRE(response->region.regionSize == region.regionSize);
    REQUIRE(response->region.protect == 0x20);
    REQUIRE(response->region.state == 0x1000);
    REQUIRE(response->region.type == 0x1000000);

    const auto forwarded = backend.virtualQueryState()->lastRequest;
    REQUIRE(forwarded.has_value());
    REQUIRE(forwarded->pid == 4321);
    REQUIRE(forwarded->address == 0x00007ff700001008ull);
}

TEST_CASE("server maps an address without a region to NotFound", "[server][vquery]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failVirtualQueryWith("no region at 0x1000", mpfshost::LookupStatus::NotFound);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::VirtualQueryRequest, 2, mpfshost::encodeVirtualQueryRequest({4, 0x1000}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::NotFound);
    REQUIRE(error->message == "no region covers that address");
}

TEST_CASE("server rejects a malformed virtual query payload", "[server][vquery]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::VirtualQueryRequest, 2, std::span<const std::byte> {});

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::MalformedPayload);

    // PID 0 names no process, so it never reaches the provider.
    client.send(mpfshost::MessageType::VirtualQueryRequest, 3, mpfshost::encodeVirtualQueryRequest({0, 0x1000}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto zeroPid = mpfshost::decodeErrorResponse(payload);
    REQUIRE(zeroPid.has_value());
    REQUIRE(zeroPid->code == mpfshost::ErrorCode::MalformedPayload);
    REQUIRE_FALSE(backend.virtualQueryState()->lastRequest.has_value());
}

TEST_CASE("server reports CapabilityUnavailable when the handle cannot query virtual memory", "[server][vquery]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setHandlesBare(true);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::VirtualQueryRequest, 2, mpfshost::encodeVirtualQueryRequest({4, 0x1000}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::CapabilityUnavailable);
}

TEST_CASE("server streams one page read item per page", "[server][page]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });

    std::vector<mpfshost::PageReadItem> pages;
    mpfshost::PageReadItem              first;
    first.address = 0x2000;
    first.ok      = true;
    first.data    = std::vector<std::byte>(mpfshost::kPageSize, std::byte {0x11});
    pages.push_back(first);

    mpfshost::PageReadItem second;
    second.address = 0x3000;
    second.ok      = false;
    pages.push_back(second);

    mpfshost::testing::FakeBackend backend;
    backend.setPageItems(pages);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PageReadRequest, 2, mpfshost::encodePageReadRequest({9, {{0x2000, 0x2000}}}));

    mpfshost::MessageType                type;
    std::uint64_t                        requestId {};
    std::vector<std::byte>               payload;
    std::vector<mpfshost::PageReadItem>  received;
    std::optional<mpfshost::PageReadEnd> end;

    while (true)
    {
        REQUIRE(client.receive(type, requestId, payload));
        if (type == mpfshost::MessageType::PageReadData)
        {
            const auto data = mpfshost::decodePageReadData(payload);
            REQUIRE(data.has_value());
            received.insert(received.end(), data->pages.begin(), data->pages.end());
        }
        else if (type == mpfshost::MessageType::PageReadEnd)
        {
            end = mpfshost::decodePageReadEnd(payload);
            break;
        }
        else
        {
            FAIL("unexpected frame " << static_cast<int>(type));
        }
    }

    REQUIRE(end.has_value());
    REQUIRE(end->status == 0);
    REQUIRE(end->pageCount == 2);
    REQUIRE(received.size() == 2);
    REQUIRE(received[0].address == 0x2000);
    REQUIRE(received[0].ok);
    REQUIRE(received[0].data.size() == mpfshost::kPageSize);
    REQUIRE(received[1].address == 0x3000);
    REQUIRE_FALSE(received[1].ok);

    REQUIRE(backend.pageState()->lastPid == 9);
    const auto addresses = backend.pageState()->lastAddresses;
    REQUIRE(addresses.has_value());
    REQUIRE(*addresses == std::vector<std::uint64_t> {0x2000, 0x3000});
}

TEST_CASE("server rejects a page read above the page cap", "[server][page]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PageReadRequest,
                2,
                mpfshost::encodePageReadRequest({9, {{0x10000, (mpfshost::kMaxPageCount + 1) * mpfshost::kPageSize}}}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::ResultTooLarge);
    REQUIRE_FALSE(backend.pageState()->lastAddresses.has_value());
}

TEST_CASE("server echoes the expanded prefetch page count", "[server][prefetch]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PrefetchRequest, 2, mpfshost::encodePrefetchRequest({5, {{0x1800, 0x1000}}}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::PrefetchResponse);

    const auto response = mpfshost::decodePrefetchResponse(payload);
    REQUIRE(response.has_value());
    REQUIRE(response->pageCount == 2);

    const auto addresses = backend.pageState()->lastAddresses;
    REQUIRE(addresses.has_value());
    REQUIRE(*addresses == std::vector<std::uint64_t> {0x1000, 0x2000});
}

TEST_CASE("server forwards config set requests and echoes the applied value", "[server][config]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::ConfigSetRequest,
                2,
                mpfshost::encodeConfigSetRequest({mpfshost::ConfigOption::ProcessDtb, 4, 0x1aa000}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ConfigSetResponse);
    const auto response = mpfshost::decodeConfigSetResponse(payload);
    REQUIRE(response.has_value());
    CHECK(response->value == 0x1aa000u);

    const auto last = backend.configState()->lastRequest;
    REQUIRE(last.has_value());
    CHECK(last->option == mpfshost::ConfigOption::ProcessDtb);
    CHECK(last->pid == 4u);
    CHECK(last->value == 0x1aa000u);
}

TEST_CASE("server rejects malformed config set requests", "[server][config]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;

    client.send(mpfshost::MessageType::ConfigSetRequest,
                2,
                mpfshost::encodeConfigSetRequest({mpfshost::ConfigOption::ProcessDtb, 0, 0x1aa000}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    CHECK(error->code == mpfshost::ErrorCode::MalformedPayload);

    client.send(mpfshost::MessageType::ConfigSetRequest,
                3,
                mpfshost::encodeConfigSetRequest({mpfshost::ConfigOption::RefreshAll, 4, 0}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    CHECK(error->code == mpfshost::ErrorCode::MalformedPayload);

    client.send(mpfshost::MessageType::ConfigSetRequest,
                4,
                mpfshost::encodeConfigSetRequest({mpfshost::ConfigOption::ProcessDtb, 4, 0x1aa123}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    CHECK(error->code == mpfshost::ErrorCode::InvalidRange);

    CHECK_FALSE(backend.configState()->lastRequest.has_value());
}

TEST_CASE("server reports CapabilityUnavailable for config set on a bare handle", "[server][config]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setHandlesBare(true);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::ConfigSetRequest,
                2,
                mpfshost::encodeConfigSetRequest({mpfshost::ConfigOption::RefreshAll, 0, 0}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    CHECK(error->code == mpfshost::ErrorCode::CapabilityUnavailable);
}

TEST_CASE("server streams search hits and reports the hit count", "[server][search]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setSearchHits({0x1000, 0x2000, 0x3000}, false);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    mpfshost::SearchTerm term;
    term.pattern = {std::byte {0xDE}, std::byte {0xAD}};
    term.mask    = {std::byte {0x00}, std::byte {0x00}};
    client.send(mpfshost::MessageType::MemorySearchRequest,
                2,
                mpfshost::encodeMemorySearchRequest({7, term, 1, 0x1000, 0x4000, 64}));

    std::vector<std::uint64_t>               hits;
    std::optional<mpfshost::MemorySearchEnd> end;
    while (true)
    {
        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));
        if (type == mpfshost::MessageType::MemorySearchData)
        {
            const auto data = mpfshost::decodeMemorySearchData(payload);
            REQUIRE(data.has_value());
            hits.insert(hits.end(), data->addresses.begin(), data->addresses.end());
        }
        else if (type == mpfshost::MessageType::MemorySearchEnd)
        {
            end = mpfshost::decodeMemorySearchEnd(payload);
            break;
        }
        else
        {
            FAIL("unexpected frame " << static_cast<int>(type));
        }
    }

    REQUIRE(end.has_value());
    REQUIRE(end->status == 0);
    REQUIRE(end->hitCount == 3);
    REQUIRE(end->truncated == 0);
    REQUIRE(hits == std::vector<std::uint64_t> {0x1000, 0x2000, 0x3000});
}

TEST_CASE("server marks a truncated search", "[server][search]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setSearchHits({0x1000, 0x2000}, true);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    mpfshost::SearchTerm term;
    term.pattern = {std::byte {0x41}};
    term.mask    = {std::byte {0x00}};
    client.send(
        mpfshost::MessageType::MemorySearchRequest, 2, mpfshost::encodeMemorySearchRequest({7, term, 1, 0, 0, 2}));

    mpfshost::MessageType                    type;
    std::uint64_t                            requestId {};
    std::vector<std::byte>                   payload;
    std::size_t                              dataFrames = 0;
    std::optional<mpfshost::MemorySearchEnd> end;
    while (true)
    {
        REQUIRE(client.receive(type, requestId, payload));
        if (type == mpfshost::MessageType::MemorySearchData)
        {
            ++dataFrames;
        }
        else if (type == mpfshost::MessageType::MemorySearchEnd)
        {
            end = mpfshost::decodeMemorySearchEnd(payload);
            break;
        }
        else
        {
            FAIL("unexpected frame " << static_cast<int>(type));
        }
    }

    REQUIRE(dataFrames == 1);
    REQUIRE(end.has_value());
    REQUIRE(end->status == 0);
    REQUIRE(end->hitCount == 2);
    REQUIRE(end->truncated == 1);
}

TEST_CASE("server answers a search that outlives its deadline with Timeout", "[server][search][timeout]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.searchState()->delay = std::chrono::milliseconds {400};

    mpfshost::ServerOptions options {fixture.socketPath()};
    options.requestTimeout = std::chrono::milliseconds {100};
    RunningServer server {options, targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.setReceiveTimeout(std::chrono::seconds {5});
    bindClient(client, 1, "vm-alpha");

    mpfshost::SearchTerm term;
    term.pattern = {std::byte {0x41}};
    term.mask    = {std::byte {0x00}};
    client.send(
        mpfshost::MessageType::MemorySearchRequest, 2, mpfshost::encodeMemorySearchRequest({7, term, 1, 0, 0, 8}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::Timeout);

    // No frame went out before the deadline, so the session keeps serving requests.
    backend.searchState()->delay = std::chrono::milliseconds::zero();
    client.send(
        mpfshost::MessageType::MemorySearchRequest, 3, mpfshost::encodeMemorySearchRequest({7, term, 1, 0, 0, 8}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::MemorySearchEnd);
    REQUIRE(requestId == 3);
}

TEST_CASE("server rejects a search with an inverted window or a bad alignment", "[server][search]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    mpfshost::SearchTerm term;
    term.pattern = {std::byte {0x41}};
    term.mask    = {std::byte {0x00}};

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;

    client.send(mpfshost::MessageType::MemorySearchRequest,
                2,
                mpfshost::encodeMemorySearchRequest({7, term, 1, 0x4000, 0x1000, 8}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::InvalidRange);

    client.send(
        mpfshost::MessageType::MemorySearchRequest, 3, mpfshost::encodeMemorySearchRequest({7, term, 3, 0, 0, 8}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::MalformedPayload);

    client.send(
        mpfshost::MessageType::MemorySearchRequest, 4, mpfshost::encodeMemorySearchRequest({7, term, 1, 0, 0, 0}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::MalformedPayload);

    client.send(mpfshost::MessageType::MemorySearchRequest,
                4,
                mpfshost::encodeMemorySearchRequest({7, term, 1, 0, 0, mpfshost::kMaxSearchHits + 1}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::ResultTooLarge);

    // An all-wildcard term leaves nothing for MemProcFS to index, so it is refused before the provider is asked.
    mpfshost::SearchTerm wildcards;
    wildcards.pattern = {std::byte {0x00}, std::byte {0x00}};
    wildcards.mask    = {std::byte {0xFF}, std::byte {0xFF}};
    client.send(
        mpfshost::MessageType::MemorySearchRequest, 6, mpfshost::encodeMemorySearchRequest({7, wildcards, 1, 0, 0, 8}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::MalformedPayload);

    REQUIRE_FALSE(backend.searchState()->lastRequest.has_value());
}

TEST_CASE("server forwards the physical pid to the memory providers", "[server][physical]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });

    mpfshost::testing::FakeBackend backend;
    backend.setPageItems({});
    backend.setSearchHits({}, false);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;

    client.send(mpfshost::MessageType::PageReadRequest,
                2,
                mpfshost::encodePageReadRequest({mpfshost::kPhysicalPid, {{0x1000, 0x1000}}}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::PageReadEnd);
    REQUIRE(backend.pageState()->lastPid == mpfshost::kPhysicalPid);

    client.send(mpfshost::MessageType::PrefetchRequest,
                3,
                mpfshost::encodePrefetchRequest({mpfshost::kPhysicalPid, {{0x1000, 0x1000}}}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::PrefetchResponse);
    REQUIRE(backend.pageState()->lastPid == mpfshost::kPhysicalPid);

    mpfshost::SearchTerm term;
    term.pattern = {std::byte {0xDE}, std::byte {0xAD}};
    term.mask    = {std::byte {0x00}, std::byte {0x00}};
    client.send(mpfshost::MessageType::MemorySearchRequest,
                4,
                mpfshost::encodeMemorySearchRequest({mpfshost::kPhysicalPid, term, 1, 0, 0, 8}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::MemorySearchEnd);
    REQUIRE(backend.searchState()->lastRequest.has_value());
    REQUIRE(backend.searchState()->lastRequest->pid == mpfshost::kPhysicalPid);
}

TEST_CASE("server streams one item per requested page", "[server][page]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });

    std::vector<mpfshost::PageReadItem> pages;
    pages.push_back({0x1000, true, std::vector<std::byte>(mpfshost::kPageSize, std::byte {0x11})});
    pages.push_back({0x2000, true, std::vector<std::byte>(mpfshost::kPageSize, std::byte {0x22})});

    mpfshost::testing::FakeBackend backend;
    backend.setPageItems(pages);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PageReadRequest, 2, mpfshost::encodePageReadRequest({4321, {{0x1008, 0x1FF8}}}));

    std::vector<mpfshost::PageReadItem> delivered;
    mpfshost::MessageType               type;
    std::uint64_t                       requestId {};
    std::vector<std::byte>              payload;
    bool                                sawEnd = false;
    while (client.receive(type, requestId, payload))
    {
        if (type == mpfshost::MessageType::PageReadData)
        {
            const auto data = mpfshost::decodePageReadData(payload);
            REQUIRE(data.has_value());
            delivered.insert(delivered.end(), data->pages.begin(), data->pages.end());
            continue;
        }
        if (type == mpfshost::MessageType::PageReadEnd)
        {
            const auto end = mpfshost::decodePageReadEnd(payload);
            REQUIRE(end.has_value());
            REQUIRE(end->status == 0);
            REQUIRE(end->pageCount == 2);
            sawEnd = true;
            break;
        }
        FAIL("unexpected frame in the page stream");
    }
    REQUIRE(sawEnd);
    REQUIRE(delivered.size() == 2);
    REQUIRE(delivered.at(0).address == 0x1000);
    REQUIRE(delivered.at(1).address == 0x2000);

    const auto forwarded = backend.pageState()->lastAddresses;
    REQUIRE(forwarded.has_value());
    REQUIRE(forwarded->size() == 2);
    REQUIRE(forwarded->at(0) == 0x1000);
    REQUIRE(forwarded->at(1) == 0x2000);
}

TEST_CASE("server reports an unreadable page as an unsuccessful item", "[server][page]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });

    std::vector<mpfshost::PageReadItem> pages;
    pages.push_back({0x1000, false, {}});

    mpfshost::testing::FakeBackend backend;
    backend.setPageItems(pages);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PageReadRequest, 2, mpfshost::encodePageReadRequest({4, {{0x1000, 0x1000}}}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::PageReadData);
    const auto data = mpfshost::decodePageReadData(payload);
    REQUIRE(data.has_value());
    REQUIRE(data->pages.size() == 1);
    REQUIRE_FALSE(data->pages.at(0).ok);

    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::PageReadEnd);
    const auto end = mpfshost::decodePageReadEnd(payload);
    REQUIRE(end.has_value());
    REQUIRE(end->status == 0);
    REQUIRE(end->pageCount == 1);
}

TEST_CASE("server rejects a page read above the page cap before using the handle", "[server][page]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PageReadRequest,
                2,
                mpfshost::encodePageReadRequest({4, {{0, (mpfshost::kMaxPageCount + 1) * mpfshost::kPageSize}}}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::ResultTooLarge);
    REQUIRE_FALSE(backend.pageState()->lastAddresses.has_value());
}

TEST_CASE("server rejects an empty page range", "[server][page]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PageReadRequest, 2, mpfshost::encodePageReadRequest({4, {{0x1000, 0}}}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::InvalidRange);
}

TEST_CASE("server answers a prefetch with the expanded page count", "[server][prefetch]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PrefetchRequest, 2, mpfshost::encodePrefetchRequest({4, {{0x1800, 0x2800}}}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::PrefetchResponse);
    const auto response = mpfshost::decodePrefetchResponse(payload);
    REQUIRE(response.has_value());
    REQUIRE(response->pageCount == 3);

    const auto forwarded = backend.pageState()->lastAddresses;
    REQUIRE(forwarded.has_value());
    REQUIRE(forwarded->size() == 3);
    REQUIRE(forwarded->at(0) == 0x1000);
    REQUIRE(forwarded->at(2) == 0x3000);
}

TEST_CASE("server reports CapabilityUnavailable for page reads and prefetch on a bare handle", "[server][page]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setHandlesBare(true);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;

    client.send(mpfshost::MessageType::PageReadRequest, 2, mpfshost::encodePageReadRequest({4, {{0x1000, 0x1000}}}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::CapabilityUnavailable);

    client.send(mpfshost::MessageType::PrefetchRequest, 3, mpfshost::encodePrefetchRequest({4, {{0x1000, 0x1000}}}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::CapabilityUnavailable);
}

TEST_CASE("server streams handles with their type and text", "[server][handles]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::HandleDescriptor file;
    file.vaObject      = 0xFFFFA00000001000ull;
    file.handle        = 0x1c;
    file.grantedAccess = 0x1F0003;
    file.typeIndex     = 7;
    file.handleCount   = 3;
    file.poolTag       = 0x466C7462;
    file.text          = "\\Device\\HarddiskVolume1";
    file.type          = "File";

    mpfshost::HandleDescriptor event;
    event.handle = 0x20;
    event.type   = "Event";

    backend.handleListState()->handles = {file, event};
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::HandleListRequest, 2, mpfshost::encodeHandleListRequest({4321, ""}));

    std::vector<mpfshost::HandleDescriptor> delivered;
    std::uint64_t                           endCount = 0;
    while (true)
    {
        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(requestId == 2);

        if (type == mpfshost::MessageType::HandleListEnd)
        {
            const auto end = mpfshost::decodeHandleListEnd(payload);
            REQUIRE(end.has_value());
            REQUIRE(end->status == 0);
            REQUIRE(end->truncated == 0);
            endCount = end->handleCount;
            break;
        }

        REQUIRE(type == mpfshost::MessageType::HandleListData);
        const auto data = mpfshost::decodeHandleListData(payload);
        REQUIRE(data.has_value());
        delivered.insert(delivered.end(), data->handles.begin(), data->handles.end());
    }

    REQUIRE(endCount == 2);
    REQUIRE(delivered.size() == 2);
    REQUIRE(delivered[0].handle == 0x1c);
    REQUIRE(delivered[0].vaObject == 0xFFFFA00000001000ull);
    REQUIRE(delivered[0].grantedAccess == 0x1F0003);
    REQUIRE(delivered[0].typeIndex == 7);
    REQUIRE(delivered[0].handleCount == 3);
    REQUIRE(delivered[0].poolTag == 0x466C7462);
    REQUIRE(delivered[0].type == "File");
    REQUIRE(delivered[1].text.empty());
    REQUIRE(delivered[1].type == "Event");

    const auto last = backend.handleListState()->lastRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->pid == 4321);
    REQUIRE(last->filter.empty());
}

TEST_CASE("server filters handles on the host and reports truncation", "[server][handles]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::HandleDescriptor file;
    file.text = "\\Device\\HarddiskVolume1";
    file.type = "File";
    mpfshost::HandleDescriptor event;
    event.type = "Event";

    backend.handleListState()->handles   = {file, event};
    backend.handleListState()->truncated = true;
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::HandleListRequest, 2, mpfshost::encodeHandleListRequest({4321, "volume"}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::HandleListData);
    const auto data = mpfshost::decodeHandleListData(payload);
    REQUIRE(data.has_value());
    REQUIRE(data->handles.size() == 1);
    REQUIRE(data->handles[0].type == "File");

    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::HandleListEnd);
    const auto end = mpfshost::decodeHandleListEnd(payload);
    REQUIRE(end.has_value());
    REQUIRE(end->handleCount == 1);
    REQUIRE(end->truncated == 1);
}

TEST_CASE("server refuses handle listings without a process id", "[server][handles]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::HandleListRequest, 2, mpfshost::encodeHandleListRequest({0, ""}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::MalformedPayload);
}

TEST_CASE("server maps a missing process in a handle listing to NotFound", "[server][handles]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.handleListState()->failure = "process 999 is not present";
    backend.handleListState()->status  = mpfshost::LookupStatus::NotFound;
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::HandleListRequest, 2, mpfshost::encodeHandleListRequest({999, ""}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::NotFound);
}

TEST_CASE("server streams a module's exports and imports", "[server][module-table]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::ExportDescriptor exportEntry;
    exportEntry.vaFunction              = 0x7FF800001000ull;
    exportEntry.ordinal                 = 12;
    exportEntry.function                = "NtCreateFile";
    backend.moduleTableState()->exports = {exportEntry};

    mpfshost::ImportDescriptor importEntry;
    importEntry.vaFunction              = 0x7FF800002000ull;
    importEntry.is32Bit                 = true;
    importEntry.hint                    = 42;
    importEntry.function                = "Sleep";
    importEntry.module                  = "kernel32.dll";
    backend.moduleTableState()->imports = {importEntry};

    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::ExportListRequest, 2, mpfshost::encodeExportListRequest({4321, "ntdll.dll"}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ExportListData);
    const auto exportData = mpfshost::decodeExportListData(payload);
    REQUIRE(exportData.has_value());
    REQUIRE(exportData->exports.size() == 1);
    REQUIRE(exportData->exports[0].function == "NtCreateFile");
    REQUIRE(exportData->exports[0].ordinal == 12);

    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ExportListEnd);
    const auto exportEnd = mpfshost::decodeExportListEnd(payload);
    REQUIRE(exportEnd.has_value());
    REQUIRE(exportEnd->exportCount == 1);

    client.send(mpfshost::MessageType::ImportListRequest, 3, mpfshost::encodeImportListRequest({4321, "ntdll.dll"}));

    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ImportListData);
    const auto importData = mpfshost::decodeImportListData(payload);
    REQUIRE(importData.has_value());
    REQUIRE(importData->imports.size() == 1);
    REQUIRE(importData->imports[0].is32Bit);
    REQUIRE(importData->imports[0].hint == 42);
    REQUIRE(importData->imports[0].module == "kernel32.dll");

    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ImportListEnd);
    const auto importEnd = mpfshost::decodeImportListEnd(payload);
    REQUIRE(importEnd.has_value());
    REQUIRE(importEnd->importCount == 1);

    REQUIRE(backend.moduleTableState()->lastExportRequest.has_value());
    REQUIRE(backend.moduleTableState()->lastExportRequest->module == "ntdll.dll");
    REQUIRE(backend.moduleTableState()->lastImportRequest.has_value());
    REQUIRE(backend.moduleTableState()->lastImportRequest->pid == 4321);
}

TEST_CASE("server rejects an export listing without a module", "[server][module-table]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    for (const auto typeRequest : {mpfshost::MessageType::ExportListRequest, mpfshost::MessageType::ImportListRequest})
    {
        if (typeRequest == mpfshost::MessageType::ExportListRequest)
        {
            client.send(typeRequest, 2, mpfshost::encodeExportListRequest({4321, ""}));
        }
        else
        {
            client.send(typeRequest, 3, mpfshost::encodeImportListRequest({4321, ""}));
        }

        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(type == mpfshost::MessageType::ErrorResponse);
        const auto error = mpfshost::decodeErrorResponse(payload);
        REQUIRE(error.has_value());
        REQUIRE(error->code == mpfshost::ErrorCode::MalformedPayload);
    }
}

TEST_CASE("server maps an unloaded module listing to NotFound for a missing module", "[server][module-table]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.moduleTableState()->exportFailure = "module 'gone.dll' is not loaded";
    backend.moduleTableState()->exportStatus  = mpfshost::LookupStatus::NotFound;
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::ExportListRequest, 2, mpfshost::encodeExportListRequest({4321, "gone.dll"}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::NotFound);
}

TEST_CASE("server streams unloaded modules and reports truncation", "[server][unloaded-modules]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::UnloadedModuleDescriptor first;
    first.vaBase    = 0x7FF800010000ull;
    first.imageSize = 0x12000;
    first.checksum  = 0xABCDEF;
    first.text      = "gone.dll";

    mpfshost::UnloadedModuleDescriptor second;
    second.text = "other.exe";

    backend.unloadedModuleState()->modules   = {first, second};
    backend.unloadedModuleState()->truncated = true;
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(
        mpfshost::MessageType::UnloadedModuleListRequest, 2, mpfshost::encodeUnloadedModuleListRequest({4321, ""}));

    std::vector<mpfshost::UnloadedModuleDescriptor> delivered;
    std::uint64_t                                   endCount = 0;
    std::uint8_t                                    truncated {};
    while (true)
    {
        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));

        if (type == mpfshost::MessageType::UnloadedModuleListEnd)
        {
            const auto end = mpfshost::decodeUnloadedModuleListEnd(payload);
            REQUIRE(end.has_value());
            REQUIRE(end->status == 0);
            endCount  = end->moduleCount;
            truncated = end->truncated;
            break;
        }

        REQUIRE(type == mpfshost::MessageType::UnloadedModuleListData);
        const auto data = mpfshost::decodeUnloadedModuleListData(payload);
        REQUIRE(data.has_value());
        delivered.insert(delivered.end(), data->modules.begin(), data->modules.end());
    }

    REQUIRE(endCount == 2);
    REQUIRE(truncated == 1);
    REQUIRE(delivered.size() == 2);
    REQUIRE(delivered[0].vaBase == 0x7FF800010000ull);
    REQUIRE(delivered[0].imageSize == 0x12000);
    REQUIRE(delivered[0].checksum == 0xABCDEF);
    REQUIRE(delivered[0].text == "gone.dll");
    REQUIRE(delivered[1].text == "other.exe");

    const auto last = backend.unloadedModuleState()->lastRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->pid == 4321);
}

TEST_CASE("server streams services and reports truncation", "[server][services]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::ServiceDescriptor first;
    first.objectAddress = 0xFFFF800012340000ull;
    first.ordinal       = 7;
    first.startType     = 2;
    first.serviceType   = 0x20;
    first.state         = 4;
    first.win32ExitCode = 0x400;
    first.checkPoint    = 3;
    first.waitHint      = 2000;
    first.pid           = 0;
    first.name          = "DemoSvc";
    first.displayName   = "Demo Service";
    first.imagePath     = "C:\\Windows\\demo.sys";
    first.path          = "\\Registry\\Machine\\System\\DemoSvc";
    first.userType      = "Driver";
    first.account       = "LocalSystem";

    mpfshost::ServiceDescriptor second;
    second.name = "OtherSvc";

    backend.serviceState()->services  = {first, second};
    backend.serviceState()->truncated = true;
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::ServiceListRequest, 2, mpfshost::encodeServiceListRequest({"svc"}));

    std::vector<mpfshost::ServiceDescriptor> delivered;
    std::uint64_t                            endCount = 0;
    std::uint8_t                             truncated {};
    while (true)
    {
        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(requestId == 2);

        if (type == mpfshost::MessageType::ServiceListEnd)
        {
            const auto end = mpfshost::decodeServiceListEnd(payload);
            REQUIRE(end.has_value());
            REQUIRE(end->status == 0);
            endCount  = end->serviceCount;
            truncated = end->truncated;
            break;
        }

        REQUIRE(type == mpfshost::MessageType::ServiceListData);
        const auto data = mpfshost::decodeServiceListData(payload);
        REQUIRE(data.has_value());
        delivered.insert(delivered.end(), data->services.begin(), data->services.end());
    }

    REQUIRE(endCount == 2);
    REQUIRE(truncated == 1);
    REQUIRE(delivered.size() == 2);
    REQUIRE(delivered[0].objectAddress == 0xFFFF800012340000ull);
    REQUIRE(delivered[0].ordinal == 7);
    REQUIRE(delivered[0].startType == 2);
    REQUIRE(delivered[0].state == 4);
    REQUIRE(delivered[0].win32ExitCode == 0x400);
    REQUIRE(delivered[0].name == "DemoSvc");
    REQUIRE(delivered[0].account == "LocalSystem");

    const auto last = backend.serviceState()->lastRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->filter == "svc");
}

TEST_CASE("server streams users and reports truncation", "[server][users]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    const mpfshost::UserDescriptor first {"Administrator", 0xFFFFA0001000ull, "S-1-5-21-1"};
    backend.userState()->users     = {first};
    backend.userState()->truncated = true;
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::UserListRequest, 2, mpfshost::encodeUserListRequest({""}));

    std::vector<mpfshost::UserDescriptor> delivered;
    std::uint64_t                         endCount = 0;
    while (true)
    {
        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(requestId == 2);

        if (type == mpfshost::MessageType::UserListEnd)
        {
            const auto end = mpfshost::decodeUserListEnd(payload);
            REQUIRE(end.has_value());
            REQUIRE(end->status == 0);
            endCount = end->userCount;
            REQUIRE(end->truncated == 1);
            break;
        }

        REQUIRE(type == mpfshost::MessageType::UserListData);
        const auto data = mpfshost::decodeUserListData(payload);
        REQUIRE(data.has_value());
        delivered.insert(delivered.end(), data->users.begin(), data->users.end());
    }

    REQUIRE(endCount == 1);
    REQUIRE(delivered.size() == 1);
    REQUIRE(delivered[0].name == "Administrator");
    REQUIRE(delivered[0].regHive == 0xFFFFA0001000ull);
    REQUIRE(delivered[0].sid == "S-1-5-21-1");
}

TEST_CASE("server streams connections scoped to one process", "[server][net]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::NetworkEntryDescriptor first;
    first.pid            = 4321;
    first.process        = "demo.exe";
    first.state          = 2;
    first.addressFamily  = 2;
    first.local.valid    = true;
    first.local.address  = "10.0.0.5:445";
    first.remote.valid   = true;
    first.remote.address = "10.0.0.9:50000";
    first.objectAddress  = 0xFFFF900000001000ull;
    first.createdTime    = 0x01D0000000000000ull;
    first.poolTag        = 0x546E6F43;

    mpfshost::NetworkEntryDescriptor second;
    second.pid     = 777;
    second.process = "other.exe";

    backend.networkState()->entries = {first, second};
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::NetworkListRequest, 2, mpfshost::encodeNetworkListRequest({4321, ""}));

    std::vector<mpfshost::NetworkEntryDescriptor> delivered;
    std::uint64_t                                 endCount = 0;
    while (true)
    {
        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(requestId == 2);

        if (type == mpfshost::MessageType::NetworkListEnd)
        {
            const auto end = mpfshost::decodeNetworkListEnd(payload);
            REQUIRE(end.has_value());
            REQUIRE(end->status == 0);
            REQUIRE(end->truncated == 0);
            endCount = end->entryCount;
            break;
        }

        REQUIRE(type == mpfshost::MessageType::NetworkListData);
        const auto data = mpfshost::decodeNetworkListData(payload);
        REQUIRE(data.has_value());
        delivered.insert(delivered.end(), data->entries.begin(), data->entries.end());
    }

    REQUIRE(endCount == 1);
    REQUIRE(delivered.size() == 1);
    REQUIRE(delivered[0].pid == 4321);
    REQUIRE(delivered[0].process == "demo.exe");
    REQUIRE(delivered[0].local.address == "10.0.0.5:445");
    REQUIRE(delivered[0].remote.address == "10.0.0.9:50000");
    REQUIRE(delivered[0].poolTag == 0x546E6F43);

    const auto last = backend.networkState()->lastRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->pid == 4321);
}

TEST_CASE("server reports CapabilityUnavailable for the system maps on a bare handle", "[server][system-maps]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setHandlesBare(true);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    struct Request
    {
        mpfshost::MessageType  type;
        std::uint64_t          requestId;
        std::vector<std::byte> payload;
    };

    const Request requests[] = {
        {mpfshost::MessageType::ServiceListRequest, 2,    mpfshost::encodeServiceListRequest({""})},
        {   mpfshost::MessageType::UserListRequest, 3,       mpfshost::encodeUserListRequest({""})},
        {mpfshost::MessageType::NetworkListRequest, 4, mpfshost::encodeNetworkListRequest({0, ""})},
        {mpfshost::MessageType::KDriverListRequest, 5,    mpfshost::encodeKDriverListRequest({""})},
        {mpfshost::MessageType::KDeviceListRequest, 6,    mpfshost::encodeKDeviceListRequest({""})},
        {mpfshost::MessageType::KObjectListRequest, 7,    mpfshost::encodeKObjectListRequest({""})}
    };
    for (const auto& request : requests)
    {
        client.send(request.type, request.requestId, request.payload);

        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(type == mpfshost::MessageType::ErrorResponse);
        const auto error = mpfshost::decodeErrorResponse(payload);
        REQUIRE(error.has_value());
        REQUIRE(error->code == mpfshost::ErrorCode::CapabilityUnavailable);
    }
}

TEST_CASE("server rejects malformed system map requests", "[server][system-maps]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    const std::vector<std::byte> garbage {std::byte {0xFF}, std::byte {0xFF}};
    const mpfshost::MessageType  types[]   = {mpfshost::MessageType::ServiceListRequest,
                                              mpfshost::MessageType::UserListRequest,
                                              mpfshost::MessageType::NetworkListRequest,
                                              mpfshost::MessageType::KDriverListRequest,
                                              mpfshost::MessageType::KDeviceListRequest,
                                              mpfshost::MessageType::KObjectListRequest};
    std::uint64_t                requestId = 2;
    for (const auto type : types)
    {
        client.send(type, requestId, garbage);

        mpfshost::MessageType  responseType;
        std::uint64_t          responseId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(responseType, responseId, payload));
        REQUIRE(responseType == mpfshost::MessageType::ErrorResponse);
        REQUIRE(responseId == requestId);
        const auto error = mpfshost::decodeErrorResponse(payload);
        REQUIRE(error.has_value());
        REQUIRE(error->code == mpfshost::ErrorCode::MalformedPayload);
        ++requestId;
    }
}

TEST_CASE("server rejects a kernel map request before bind", "[server][kernel-maps]")
{
    ServerFixture                       fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    RunningServer                       server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    client.send(mpfshost::MessageType::KDriverListRequest, 21, mpfshost::encodeKDriverListRequest({}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ErrorResponse);
    REQUIRE(requestId == 21);
    const auto error = mpfshost::decodeErrorResponse(payload);
    REQUIRE(error.has_value());
    REQUIRE(error->code == mpfshost::ErrorCode::NotBound);

    client.send(mpfshost::MessageType::ListTargetsRequest, 22);
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ListTargetsResponse);
}
