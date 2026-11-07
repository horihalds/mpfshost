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
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace
{
    class SessionFixture
    {
    public:
        SessionFixture()
        {
            std::filesystem::create_directories(MPFSHOST_TEST_TMP_DIR);
            std::string pattern = (std::filesystem::path {MPFSHOST_TEST_TMP_DIR} / "pool-pte-pfn-XXXXXX").string();
            REQUIRE(::mkdtemp(pattern.data()) != nullptr);
            _root       = pattern;
            _socketPath = _root / "host.sock";
            REQUIRE(_socketPath.string().size() < sizeof(sockaddr_un::sun_path));
        }

        ~SessionFixture()
        {
            std::error_code error;
            std::filesystem::remove_all(_root, error);
        }

        SessionFixture(const SessionFixture&)            = delete;
        SessionFixture& operator=(const SessionFixture&) = delete;

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

    class RunningServer
    {
    public:
        RunningServer(const std::filesystem::path&  socketPath,
                      const mpfshost::TargetSource& targets,
                      mpfshost::VmBackend&          backend)
            : _server {mpfshost::ServerOptions {socketPath}, targets, backend},
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

    void bindClient(ClientSocket& client, std::uint64_t requestId, const std::string& targetName)
    {
        client.send(mpfshost::MessageType::BindRequest, requestId, mpfshost::encodeBindRequest({targetName}));

        mpfshost::MessageType  type;
        std::uint64_t          responseId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, responseId, payload));
        REQUIRE(type == mpfshost::MessageType::BindResponse);
    }

    mpfshost::ErrorCode receiveErrorCode(ClientSocket& client)
    {
        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(type == mpfshost::MessageType::ErrorResponse);
        const auto error = mpfshost::decodeErrorResponse(payload);
        REQUIRE(error.has_value());
        return error->code;
    }

    mpfshost::PteDescriptor makePte(std::uint64_t index, std::string text = {})
    {
        mpfshost::PteDescriptor region;
        region.vaBase = 0x10000 + index * 0x1000;
        region.pages  = 2;
        region.text   = std::move(text);
        return region;
    }

    mpfshost::PoolDescriptor makePool(std::uint64_t index)
    {
        mpfshost::PoolDescriptor entry;
        entry.va    = 0xFFFF800000000000ULL + index * 0x100;
        entry.bytes = 0x100;
        entry.tag   = static_cast<std::uint32_t>('F') | static_cast<std::uint32_t>('i') << 8
                    | static_cast<std::uint32_t>('l') << 16 | static_cast<std::uint32_t>('e') << 24;
        return entry;
    }

    mpfshost::PfnDescriptor makePfn(std::uint32_t index)
    {
        mpfshost::PfnDescriptor entry;
        entry.pfn = index;
        entry.pid = 4;
        entry.va  = 0x1000 + static_cast<std::uint64_t>(index) * 0x1000;
        return entry;
    }
} // namespace

TEST_CASE("client lists pte regions through the host", "[ipc][client][pte]")
{
    SessionFixture                      fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setPteEntries({makePte(0, "ntdll.dll"), makePte(1, "kernel32.dll")});
    backend.pteState()->mapEntries = 5;
    backend.pteState()->truncated  = true;
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  result =
        mpfshost::listPte(fixture.socketPath(), {"vm-alpha", 4321, true, "ntdll", 0x10000, 0x7FFFFFFF0000ULL}, error);

    REQUIRE(result.has_value());
    REQUIRE(error.empty());
    REQUIRE(result->regions.size() == 1);
    REQUIRE(result->regions[0].text == "ntdll.dll");
    REQUIRE(result->mapEntries == 5);
    REQUIRE(result->truncated);

    const auto last = backend.pteState()->lastRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->pid == 4321);
    REQUIRE(last->identifyModules == 1);
    REQUIRE(last->filter == "ntdll");
    REQUIRE(last->minAddress == 0x10000);
    REQUIRE(last->maxAddress == 0x7FFFFFFF0000ULL);
}

TEST_CASE("client lists pool entries through the host", "[ipc][client][pool]")
{
    SessionFixture                      fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setPoolEntries({makePool(0), makePool(1)});
    backend.poolState()->mapEntries = 2;
    backend.poolState()->mapBytes   = 0x123456;
    backend.poolState()->truncated  = true;
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  result = mpfshost::listPool(fixture.socketPath(), {"vm-alpha", true, "Fi", 0x100, 0, 0}, error);

    REQUIRE(result.has_value());
    REQUIRE(error.empty());
    REQUIRE(result->entries.size() == 2);
    REQUIRE(result->mapEntries == 2);
    REQUIRE(result->mapBytes == 0x123456);
    REQUIRE(result->truncated);

    const auto last = backend.poolState()->lastRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->big == 1);
    REQUIRE(last->tagFilter == "Fi");
    REQUIRE(last->minBytes == 0x100);
}

TEST_CASE("client lists pool tags through the host", "[ipc][client][pool]")
{
    SessionFixture                      fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::PoolTagDescriptor fileTag;
    fileTag.tag     = static_cast<std::uint32_t>('F') | static_cast<std::uint32_t>('i') << 8
                    | static_cast<std::uint32_t>('l') << 16 | static_cast<std::uint32_t>('e') << 24;
    fileTag.entries = 12;
    mpfshost::PoolTagDescriptor mmTag;
    mmTag.tag     = static_cast<std::uint32_t>('M') | static_cast<std::uint32_t>('m') << 8
                  | static_cast<std::uint32_t>('S') << 16 | static_cast<std::uint32_t>('t') << 24;
    mmTag.entries = 3;
    backend.setPoolTags({fileTag, mmTag});
    backend.poolState()->mapTags    = 7;
    backend.poolState()->mapEntries = 15;
    backend.poolState()->truncated  = true;
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  result = mpfshost::listPoolTags(fixture.socketPath(), {"vm-alpha", true, "F"}, error);

    REQUIRE(result.has_value());
    REQUIRE(error.empty());
    REQUIRE(result->tags.size() == 1);
    REQUIRE(result->tags[0].entries == 12);
    REQUIRE(result->mapTags == 7);
    REQUIRE(result->mapEntries == 15);
    REQUIRE(result->truncated);

    const auto last = backend.poolState()->lastTagRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->big == 1);
    REQUIRE(last->tagFilter == "F");
}

TEST_CASE("client lists pfn entries through the host", "[ipc][client][pfn]")
{
    SessionFixture                      fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setPfnEntries({makePfn(0x1F000), makePfn(0x1F001)});
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  result = mpfshost::listPfn(fixture.socketPath(), {"vm-alpha", 0x1F000, 64}, error);

    REQUIRE(result.has_value());
    REQUIRE(error.empty());
    REQUIRE(result->entries.size() == 2);
    REQUIRE(result->entries[0].pfn == 0x1F000);
    REQUIRE(result->entries[1].pfn == 0x1F001);

    const auto last = backend.pfnState()->lastRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->pfnStart == 0x1F000);
    REQUIRE(last->pfnCount == 64);
}

TEST_CASE("pte listing streams multiple data frames", "[ipc][server][pte]")
{
    SessionFixture                      fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    std::vector<mpfshost::PteDescriptor> regions(20000);
    for (std::uint64_t index = 0; index < regions.size(); ++index)
    {
        regions[index] = makePte(index);
    }
    backend.setPteEntries(regions);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PteListRequest, 2, mpfshost::encodePteListRequest({0, 0, 0, 0, ""}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    std::size_t            dataFrames = 0;
    while (true)
    {
        REQUIRE(client.receive(type, requestId, payload));
        if (type == mpfshost::MessageType::PteListEnd)
        {
            break;
        }
        REQUIRE(type == mpfshost::MessageType::PteListData);
        ++dataFrames;
    }
    REQUIRE(dataFrames > 1);

    const auto end = mpfshost::decodePteListEnd(payload);
    REQUIRE(end.has_value());
    REQUIRE(end->entryCount == regions.size());
}

TEST_CASE("pool listing streams multiple data frames", "[ipc][server][pool]")
{
    SessionFixture                      fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    std::vector<mpfshost::PoolDescriptor> entries(30000);
    for (std::uint64_t index = 0; index < entries.size(); ++index)
    {
        entries[index] = makePool(index);
    }
    backend.setPoolEntries(entries);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PoolListRequest, 2, mpfshost::encodePoolListRequest({}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    std::size_t            dataFrames = 0;
    while (true)
    {
        REQUIRE(client.receive(type, requestId, payload));
        if (type == mpfshost::MessageType::PoolListEnd)
        {
            break;
        }
        REQUIRE(type == mpfshost::MessageType::PoolListData);
        ++dataFrames;
    }
    REQUIRE(dataFrames > 1);

    const auto end = mpfshost::decodePoolListEnd(payload);
    REQUIRE(end.has_value());
    REQUIRE(end->entryCount == entries.size());
}

TEST_CASE("pfn listing streams multiple data frames", "[ipc][server][pfn]")
{
    SessionFixture                      fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    std::vector<mpfshost::PfnDescriptor> entries(15000);
    for (std::uint32_t index = 0; index < entries.size(); ++index)
    {
        entries[index] = makePfn(index);
    }
    backend.setPfnEntries(entries);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PfnListRequest, 2, mpfshost::encodePfnListRequest({0, 15000}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    std::size_t            dataFrames = 0;
    while (true)
    {
        REQUIRE(client.receive(type, requestId, payload));
        if (type == mpfshost::MessageType::PfnListEnd)
        {
            break;
        }
        REQUIRE(type == mpfshost::MessageType::PfnListData);
        ++dataFrames;
    }
    REQUIRE(dataFrames > 1);

    const auto end = mpfshost::decodePfnListEnd(payload);
    REQUIRE(end.has_value());
    REQUIRE(end->entryCount == entries.size());
}

TEST_CASE("pte listing maps a missing pid to NotFound", "[ipc][server][pte]")
{
    SessionFixture                      fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failPteWith("process 4321 is not present", mpfshost::LookupStatus::NotFound);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PteListRequest, 2, mpfshost::encodePteListRequest({4321, 0, 0, 0, ""}));

    REQUIRE(receiveErrorCode(client) == mpfshost::ErrorCode::NotFound);
}

TEST_CASE("pool and pfn listing map a provider failure to Internal", "[ipc][server][pool][pfn]")
{
    SessionFixture                      fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failPoolWith("pool map failed");
    backend.failPfnWith("pfn map failed");
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::PoolListRequest, 2, mpfshost::encodePoolListRequest({}));
    REQUIRE(receiveErrorCode(client) == mpfshost::ErrorCode::Internal);

    client.send(mpfshost::MessageType::PfnListRequest, 3, mpfshost::encodePfnListRequest({0, 1}));
    REQUIRE(receiveErrorCode(client) == mpfshost::ErrorCode::Internal);
}

TEST_CASE("bare handles answer CapabilityUnavailable for pte, pool, pool tags and pfn", "[ipc][server][pte][pool][pfn]")
{
    SessionFixture                      fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setHandlesBare(true);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::PteListRequest, 2, mpfshost::encodePteListRequest({}));
    REQUIRE(receiveErrorCode(client) == mpfshost::ErrorCode::CapabilityUnavailable);

    client.send(mpfshost::MessageType::PoolListRequest, 3, mpfshost::encodePoolListRequest({}));
    REQUIRE(receiveErrorCode(client) == mpfshost::ErrorCode::CapabilityUnavailable);

    client.send(mpfshost::MessageType::PoolTagListRequest, 4, mpfshost::encodePoolTagListRequest({}));
    REQUIRE(receiveErrorCode(client) == mpfshost::ErrorCode::CapabilityUnavailable);

    client.send(mpfshost::MessageType::PfnListRequest, 5, mpfshost::encodePfnListRequest({0, 1}));
    REQUIRE(receiveErrorCode(client) == mpfshost::ErrorCode::CapabilityUnavailable);
}

TEST_CASE("pfn listing rejects a count of zero and above the cap", "[ipc][server][pfn]")
{
    SessionFixture                      fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::PfnListRequest, 2, mpfshost::encodePfnListRequest({0, 0}));
    REQUIRE(receiveErrorCode(client) == mpfshost::ErrorCode::MalformedPayload);

    client.send(mpfshost::MessageType::PfnListRequest,
                3,
                mpfshost::encodePfnListRequest({0, static_cast<std::uint32_t>(mpfshost::kMaxPfnEntries) + 1}));
    REQUIRE(receiveErrorCode(client) == mpfshost::ErrorCode::MalformedPayload);
}
