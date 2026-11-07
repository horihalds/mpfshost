#include <catch2/catch.hpp>

#include "core/ipc/frame.hpp"
#include "core/ipc/message.hpp"
#include "core/server/physical_memory_provider.hpp"
#include "core/server/server.hpp"
#include "core/server/vm_provider.hpp"
#include "support/fake_backend.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace
{
    class PhysicalFixture
    {
    public:
        PhysicalFixture()
        {
            std::filesystem::create_directories(MPFSHOST_TEST_TMP_DIR);
            std::string pattern = (std::filesystem::path {MPFSHOST_TEST_TMP_DIR} / "physical-XXXXXX").string();
            REQUIRE(::mkdtemp(pattern.data()) != nullptr);
            _root       = pattern;
            _socketPath = _root / "host.sock";
            REQUIRE(_socketPath.string().size() < sizeof(sockaddr_un::sun_path));
        }

        ~PhysicalFixture()
        {
            std::error_code error;
            std::filesystem::remove_all(_root, error);
        }

        PhysicalFixture(const PhysicalFixture&)            = delete;
        PhysicalFixture& operator=(const PhysicalFixture&) = delete;

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

    class PhysicalClient
    {
    public:
        explicit PhysicalClient(const std::filesystem::path& socketPath)
        {
            _fd = connectTo(socketPath);
            REQUIRE(_fd >= 0);
        }

        ~PhysicalClient()
        {
            if (_fd >= 0)
            {
                ::close(_fd);
            }
        }

        PhysicalClient(const PhysicalClient&)            = delete;
        PhysicalClient& operator=(const PhysicalClient&) = delete;

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

    class PhysicalServer
    {
    public:
        PhysicalServer(const std::filesystem::path&  socketPath,
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

        ~PhysicalServer()
        {
            _server.requestStop();
            _thread.join();
        }

        PhysicalServer(const PhysicalServer&)            = delete;
        PhysicalServer& operator=(const PhysicalServer&) = delete;

    private:
        mpfshost::Server _server;
        std::jthread     _thread;
    };

    void bindSession(PhysicalClient& client, std::uint64_t requestId, const std::string& targetName)
    {
        client.send(mpfshost::MessageType::BindRequest, requestId, mpfshost::encodeBindRequest({targetName}));

        mpfshost::MessageType  type;
        std::uint64_t          responseId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, responseId, payload));
        REQUIRE(type == mpfshost::MessageType::BindResponse);
    }

    mpfshost::ErrorCode receiveError(PhysicalClient& client, std::uint64_t expectedRequestId)
    {
        mpfshost::MessageType  type;
        std::uint64_t          requestId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(type == mpfshost::MessageType::ErrorResponse);
        REQUIRE(requestId == expectedRequestId);

        const auto error = mpfshost::decodeErrorResponse(payload);
        REQUIRE(error.has_value());
        return error->code;
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

    mpfshost::VmDescriptor makeVm(std::string name, std::uint64_t gpaMax)
    {
        mpfshost::VmDescriptor vm;
        vm.name       = std::move(name);
        vm.gpaMax     = gpaMax;
        vm.kind       = 1;
        vm.systemKind = 2;
        vm.active     = true;
        vm.vmMemPid   = 4242;
        return vm;
    }
} // namespace

TEST_CASE("physical range validation stays inside the partition", "[physical]")
{
    REQUIRE(mpfshost::isWithinPhysicalRange(0, 0x1000, 0x2000));
    REQUIRE(mpfshost::isWithinPhysicalRange(0x1000, 0x1000, 0x2000));
    REQUIRE(mpfshost::isWithinPhysicalRange(0x2000, 0, 0x2000));
    REQUIRE_FALSE(mpfshost::isWithinPhysicalRange(0x1001, 0x1000, 0x2000));
    REQUIRE_FALSE(mpfshost::isWithinPhysicalRange(0, 0x2001, 0x2000));
    REQUIRE_FALSE(mpfshost::isWithinPhysicalRange(0x100000000ull, 0x1000, 0x2000));
}

TEST_CASE("physical scatter entries follow the MEM_SCATTER rules", "[physical]")
{
    using mpfshost::ScatterEntry;

    const ScatterEntry read {0x1000, 8, mpfshost::kScatterEntryRead, {}};
    REQUIRE(mpfshost::isValidPhysicalScatterEntry(read));

    const ScatterEntry fullPage {0x1000, 0x1000, mpfshost::kScatterEntryRead, {}};
    REQUIRE(mpfshost::isValidPhysicalScatterEntry(fullPage));

    const ScatterEntry write {0x2000, 8, mpfshost::kScatterEntryWrite, std::vector<std::byte>(8)};
    REQUIRE(mpfshost::isValidPhysicalScatterEntry(write));

    const ScatterEntry tooSmall {0x1000, 4, mpfshost::kScatterEntryRead, {}};
    REQUIRE_FALSE(mpfshost::isValidPhysicalScatterEntry(tooSmall));

    const ScatterEntry tooLarge {0x1000, 0x1004, mpfshost::kScatterEntryRead, {}};
    REQUIRE_FALSE(mpfshost::isValidPhysicalScatterEntry(tooLarge));

    const ScatterEntry unaligned {0x1002, 8, mpfshost::kScatterEntryRead, {}};
    REQUIRE_FALSE(mpfshost::isValidPhysicalScatterEntry(unaligned));

    const ScatterEntry crossingPage {0x1FFC, 8, mpfshost::kScatterEntryRead, {}};
    REQUIRE_FALSE(mpfshost::isValidPhysicalScatterEntry(crossingPage));

    const ScatterEntry readWithData {0x1000, 8, mpfshost::kScatterEntryRead, std::vector<std::byte>(8)};
    REQUIRE_FALSE(mpfshost::isValidPhysicalScatterEntry(readWithData));

    const ScatterEntry shortWrite {0x1000, 8, mpfshost::kScatterEntryWrite, std::vector<std::byte>(4)};
    REQUIRE_FALSE(mpfshost::isValidPhysicalScatterEntry(shortWrite));

    const ScatterEntry unknownKind {0x1000, 8, 7, {}};
    REQUIRE_FALSE(mpfshost::isValidPhysicalScatterEntry(unknownKind));
}

TEST_CASE("server lists the vm partitions of the bound target", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x4000), makeVm("", 0x2000)});
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::VmListRequest, 2);

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::VmListResponse);
    REQUIRE(requestId == 2);

    const auto response = mpfshost::decodeVmListResponse(payload);
    REQUIRE(response.has_value());
    REQUIRE(response->vms.size() == 2);
    REQUIRE(response->vms[0].name == "physical");
    REQUIRE(response->vms[0].gpaMax == 0x4000);
    REQUIRE(response->vms[0].vmMemPid == 4242);
    REQUIRE(response->vms[1].name.empty());
}

TEST_CASE("server rejects vm-list before the session is bound", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    PhysicalServer                      server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    client.send(mpfshost::MessageType::VmListRequest, 3);

    REQUIRE(receiveError(client, 3) == mpfshost::ErrorCode::NotBound);
}

TEST_CASE("server rejects vm-list on a handle without the capability", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setHandlesBare(true);
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::VmListRequest, 4);

    REQUIRE(receiveError(client, 4) == mpfshost::ErrorCode::CapabilityUnavailable);
}

TEST_CASE("server rejects a vm-list payload", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    PhysicalServer                 server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");

    const std::vector<std::byte> payload {std::byte {1}};
    client.send(mpfshost::MessageType::VmListRequest, 5, payload);

    REQUIRE(receiveError(client, 5) == mpfshost::ErrorCode::MalformedPayload);
}

TEST_CASE("server surfaces a failed vm listing", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failVmsWith("map unavailable");
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::VmListRequest, 6);

    REQUIRE(receiveError(client, 6) == mpfshost::ErrorCode::Internal);
}

TEST_CASE("server translates a gpa inside the partition", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x4000)});
    backend.physicalState()->translateResponse = {0x1234, 0x7FF600001000ull, true, true};
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::GpaTranslateRequest, 7, mpfshost::encodeGpaTranslateRequest({0, 0x10}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::GpaTranslateResponse);
    REQUIRE(requestId == 7);

    const auto response = mpfshost::decodeGpaTranslateResponse(payload);
    REQUIRE(response.has_value());
    REQUIRE(response->pa == 0x1234);
    REQUIRE(response->va == 0x7FF600001000ull);
    REQUIRE(response->paPresent);
    REQUIRE(response->vaPresent);

    const auto state = backend.physicalState();
    REQUIRE(state->lastTranslateVmIndex == 0);
    REQUIRE(state->lastTranslateGpa == 0x10);
}

TEST_CASE("server reports an unknown vm index", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x4000)});
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::GpaTranslateRequest, 8, mpfshost::encodeGpaTranslateRequest({4, 0}));

    REQUIRE(receiveError(client, 8) == mpfshost::ErrorCode::UnknownVm);
}

TEST_CASE("server rejects a gpa translate outside the partition", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x4000)});
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::GpaTranslateRequest, 9, mpfshost::encodeGpaTranslateRequest({0, 0x4000}));

    REQUIRE(receiveError(client, 9) == mpfshost::ErrorCode::InvalidRange);
    REQUIRE(backend.physicalState()->lastTranslateGpa == 0);
}

TEST_CASE("server surfaces a failed gpa translation", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x4000)});
    backend.physicalState()->translateFailure = "translation failed";
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::GpaTranslateRequest, 10, mpfshost::encodeGpaTranslateRequest({0, 0}));

    REQUIRE(receiveError(client, 10) == mpfshost::ErrorCode::Internal);
}

TEST_CASE("server translates a process virtual address to a physical one", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.memoryState()->virt2PhysPa = 0x700002000ull;
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::Virt2PhysRequest, 11, mpfshost::encodeVirt2PhysRequest({4242, 0xFFFF0000}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::Virt2PhysResponse);
    REQUIRE(requestId == 11);

    const auto response = mpfshost::decodeVirt2PhysResponse(payload);
    REQUIRE(response.has_value());
    REQUIRE(response->pa == 0x700002000ull);

    const auto state = backend.memoryState();
    REQUIRE(state->lastVirt2PhysPid == 4242);
    REQUIRE(state->lastVirt2PhysVa == 0xFFFF0000);
}

TEST_CASE("server surfaces a failed virt2phys translation", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.memoryState()->virt2PhysFailure = "page not present";
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::Virt2PhysRequest, 12, mpfshost::encodeVirt2PhysRequest({4242, 0x1000}));

    REQUIRE(receiveError(client, 12) == mpfshost::ErrorCode::Internal);
}

TEST_CASE("server streams a guest physical read across several frames", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x800000)});
    PhysicalServer server {fixture.socketPath(), targets, backend};

    constexpr std::uint32_t kSize = 3 * mpfshost::kMaxPayloadLength;
    constexpr std::uint64_t kGpa  = 0x1000;

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PhysicalReadRequest, 2, mpfshost::encodePhysicalReadRequest({0, kGpa, kSize}));

    std::vector<std::byte>     assembled;
    std::vector<std::uint64_t> frameSizes;
    mpfshost::MessageType      type;
    std::uint64_t              requestId {};
    std::vector<std::byte>     payload;
    while (true)
    {
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(requestId == 2);
        REQUIRE(type != mpfshost::MessageType::ErrorResponse);
        if (type == mpfshost::MessageType::PhysicalReadEnd)
        {
            break;
        }
        REQUIRE(type == mpfshost::MessageType::PhysicalReadData);
        frameSizes.push_back(payload.size());
        assembled.insert(assembled.end(), payload.begin(), payload.end());
    }

    const auto end = mpfshost::decodePhysicalReadEnd(payload);
    REQUIRE(end.has_value());
    REQUIRE(end->status == 0);
    REQUIRE(end->bytesDelivered == kSize);
    REQUIRE(end->error.empty());

    REQUIRE(frameSizes.size() == 3);
    REQUIRE(frameSizes[0] == mpfshost::kMaxPayloadLength);
    REQUIRE(assembled == addressPattern(kGpa, kSize));

    const auto state = backend.physicalState();
    REQUIRE(state->lastReadVmIndex == 0);
    REQUIRE(state->lastReadGpa == kGpa);
    REQUIRE(state->lastReadSize == kSize);
}

TEST_CASE("server reports a short guest physical read without data frames", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x8000)});
    backend.physicalState()->shortRead = true;
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PhysicalReadRequest, 3, mpfshost::encodePhysicalReadRequest({0, 0, 0x10}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::PhysicalReadEnd);
    REQUIRE(requestId == 3);

    const auto end = mpfshost::decodePhysicalReadEnd(payload);
    REQUIRE(end.has_value());
    REQUIRE(end->status == 1);
    REQUIRE(end->bytesDelivered == 0);
    REQUIRE_FALSE(end->error.empty());
}

TEST_CASE("server surfaces a failed guest physical read", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x8000)});
    backend.physicalState()->failure = "VMMDLL_VmMemRead failed";
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PhysicalReadRequest, 4, mpfshost::encodePhysicalReadRequest({0, 0, 0x10}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::PhysicalReadEnd);

    const auto end = mpfshost::decodePhysicalReadEnd(payload);
    REQUIRE(end.has_value());
    REQUIRE(end->status == 1);
    REQUIRE(end->error == "VMMDLL_VmMemRead failed");
}

TEST_CASE("server rejects a guest physical read that leaves the partition", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x8000)});
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PhysicalReadRequest, 5, mpfshost::encodePhysicalReadRequest({0, 0x7FF0, 0x20}));

    REQUIRE(receiveError(client, 5) == mpfshost::ErrorCode::InvalidRange);
    REQUIRE(backend.physicalState()->lastReadSize == 0);
}

TEST_CASE("server accepts a guest physical read that ends exactly at gpaMax", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x8000)});
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PhysicalReadRequest, 6, mpfshost::encodePhysicalReadRequest({0, 0x7FF0, 0x10}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::PhysicalReadData);

    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::PhysicalReadEnd);
    const auto end = mpfshost::decodePhysicalReadEnd(payload);
    REQUIRE(end.has_value());
    REQUIRE(end->status == 0);
}

TEST_CASE("server rejects a zero-sized guest physical read", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x8000)});
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PhysicalReadRequest, 7, mpfshost::encodePhysicalReadRequest({0, 0, 0}));

    REQUIRE(receiveError(client, 7) == mpfshost::ErrorCode::InvalidRange);
}

TEST_CASE("server rejects a physical read for an unknown vm", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x8000)});
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PhysicalReadRequest, 8, mpfshost::encodePhysicalReadRequest({9, 0, 0x10}));

    REQUIRE(receiveError(client, 8) == mpfshost::ErrorCode::UnknownVm);
}

TEST_CASE("server rejects a physical read on a handle without the capability", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setHandlesBare(true);
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PhysicalReadRequest, 9, mpfshost::encodePhysicalReadRequest({0, 0, 0x10}));

    REQUIRE(receiveError(client, 9) == mpfshost::ErrorCode::CapabilityUnavailable);
}

TEST_CASE("server rejects a physical read before the session is bound", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    PhysicalServer                      server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    client.send(mpfshost::MessageType::PhysicalReadRequest, 10, mpfshost::encodePhysicalReadRequest({0, 0, 0x10}));

    REQUIRE(receiveError(client, 10) == mpfshost::ErrorCode::NotBound);
}

TEST_CASE("server writes guest physical memory", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x8000)});
    PhysicalServer server {fixture.socketPath(), targets, backend};

    const std::vector<std::byte> data {std::byte {0x40}, std::byte {0x41}, std::byte {0x42}};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(
        mpfshost::MessageType::PhysicalWriteRequest, 2, mpfshost::encodePhysicalWriteRequest({0, 0x2000, data}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::PhysicalWriteResponse);
    REQUIRE(requestId == 2);

    const auto response = mpfshost::decodeMemoryWriteResponse(payload);
    REQUIRE(response.has_value());
    REQUIRE(response->bytesWritten == data.size());

    const auto state = backend.physicalState();
    REQUIRE(state->lastWriteVmIndex == 0);
    REQUIRE(state->lastWriteGpa == 0x2000);
    REQUIRE(state->lastWriteData == data);
}

TEST_CASE("server rejects an empty guest physical write", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x8000)});
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PhysicalWriteRequest, 3, mpfshost::encodePhysicalWriteRequest({0, 0x2000, {}}));

    REQUIRE(receiveError(client, 3) == mpfshost::ErrorCode::InvalidRange);
}

TEST_CASE("server rejects a guest physical write that leaves the partition", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x8000)});
    PhysicalServer server {fixture.socketPath(), targets, backend};

    const std::vector<std::byte> data(0x20, std::byte {0xFF});

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(
        mpfshost::MessageType::PhysicalWriteRequest, 4, mpfshost::encodePhysicalWriteRequest({0, 0x7FF0, data}));

    REQUIRE(receiveError(client, 4) == mpfshost::ErrorCode::InvalidRange);
    REQUIRE(backend.physicalState()->lastWriteData.empty());
}

TEST_CASE("server surfaces a failed guest physical write", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x8000)});
    backend.physicalState()->writeFailure = "VMMDLL_VmMemWrite failed";
    PhysicalServer server {fixture.socketPath(), targets, backend};

    const std::vector<std::byte> data {std::byte {0x01}};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(
        mpfshost::MessageType::PhysicalWriteRequest, 5, mpfshost::encodePhysicalWriteRequest({0, 0x1000, data}));

    REQUIRE(receiveError(client, 5) == mpfshost::ErrorCode::Internal);
}

TEST_CASE("server scatters guest physical reads in one batch", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x8000)});
    PhysicalServer server {fixture.socketPath(), targets, backend};

    const mpfshost::PhysicalScatterRequest request {
        0, {{0x1000, 16, mpfshost::kScatterEntryRead, {}}, {0x2000, 8, mpfshost::kScatterEntryRead, {}}}
    };

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PhysicalScatterRequest, 2, mpfshost::encodePhysicalScatterRequest(request));

    std::vector<std::vector<std::byte>> frames;
    mpfshost::MessageType               type;
    std::uint64_t                       requestId {};
    std::vector<std::byte>              payload;
    while (true)
    {
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(requestId == 2);
        if (type == mpfshost::MessageType::PhysicalScatterEnd)
        {
            break;
        }
        REQUIRE(type == mpfshost::MessageType::PhysicalScatterData);
        frames.push_back(payload);
    }

    const auto end = mpfshost::decodeScatterExecuteEnd(payload);
    REQUIRE(end.has_value());
    REQUIRE(end->status == 0);
    REQUIRE(end->error.empty());
    REQUIRE(end->outcomes.size() == 2);
    REQUIRE(end->outcomes[0].address == 0x1000);
    REQUIRE(end->outcomes[0].requestedSize == 16);
    REQUIRE(end->outcomes[0].bytesRead == 16);
    REQUIRE(end->outcomes[0].ok == 1);
    REQUIRE(end->outcomes[1].address == 0x2000);
    REQUIRE(end->outcomes[1].ok == 1);

    REQUIRE(frames.size() == 2);
    const auto first = mpfshost::decodeScatterExecuteData(frames[0]);
    REQUIRE(first.has_value());
    REQUIRE(first->entryIndex == 0);
    REQUIRE(first->data == addressPattern(0x1000, 16));
    const auto second = mpfshost::decodeScatterExecuteData(frames[1]);
    REQUIRE(second.has_value());
    REQUIRE(second->entryIndex == 1);
    REQUIRE(second->data == addressPattern(0x2000, 8));

    const auto state = backend.physicalState();
    REQUIRE(state->lastScatterVmIndex == 0);
    REQUIRE(state->lastScatterEntries.size() == 2);
    REQUIRE(state->lastScatterEntries[1].address == 0x2000);
}

TEST_CASE("server reports a partially failed physical scatter", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x8000)});
    backend.physicalState()->failedScatterIndices = {1};
    PhysicalServer server {fixture.socketPath(), targets, backend};

    const mpfshost::PhysicalScatterRequest request {
        0, {{0x1000, 8, mpfshost::kScatterEntryRead, {}}, {0x2000, 8, mpfshost::kScatterEntryRead, {}}}
    };

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PhysicalScatterRequest, 3, mpfshost::encodePhysicalScatterRequest(request));

    std::vector<std::vector<std::byte>> frames;
    mpfshost::MessageType               type;
    std::uint64_t                       requestId {};
    std::vector<std::byte>              payload;
    while (true)
    {
        REQUIRE(client.receive(type, requestId, payload));
        if (type == mpfshost::MessageType::PhysicalScatterEnd)
        {
            break;
        }
        REQUIRE(type == mpfshost::MessageType::PhysicalScatterData);
        frames.push_back(payload);
    }

    const auto end = mpfshost::decodeScatterExecuteEnd(payload);
    REQUIRE(end.has_value());
    REQUIRE(end->status == 0);
    REQUIRE(end->outcomes.size() == 2);
    REQUIRE(end->outcomes[0].ok == 1);
    REQUIRE(end->outcomes[1].ok == 0);
    REQUIRE(frames.size() == 1);

    const auto frame = mpfshost::decodeScatterExecuteData(frames[0]);
    REQUIRE(frame.has_value());
    REQUIRE(frame->entryIndex == 0);
}

TEST_CASE("server rejects a physical scatter batch that is empty", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x8000)});
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PhysicalScatterRequest, 4, mpfshost::encodePhysicalScatterRequest({0, {}}));

    REQUIRE(receiveError(client, 4) == mpfshost::ErrorCode::MalformedPayload);
}

TEST_CASE("server rejects a physical scatter entry that breaks the MEM_SCATTER rules", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x8000)});
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");

    const std::vector<mpfshost::ScatterEntry> invalidEntries {
        {0x1000,      4,  mpfshost::kScatterEntryRead,                        {}},
        {0x1000, 0x1004,  mpfshost::kScatterEntryRead,                        {}},
        {0x1002,      8,  mpfshost::kScatterEntryRead,                        {}},
        {0x1FFC,      8,  mpfshost::kScatterEntryRead,                        {}},
        {0x1000,      8, mpfshost::kScatterEntryWrite, std::vector<std::byte>(4)},
    };

    std::uint64_t requestId = 10;
    for (const auto& entry : invalidEntries)
    {
        client.send(mpfshost::MessageType::PhysicalScatterRequest,
                    requestId,
                    mpfshost::encodePhysicalScatterRequest({0, {entry}}));
        REQUIRE(receiveError(client, requestId) == mpfshost::ErrorCode::MalformedPayload);
        ++requestId;
    }

    REQUIRE(backend.physicalState()->lastScatterEntries.empty());
}

TEST_CASE("server rejects an oversized physical scatter batch", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x8000)});
    PhysicalServer server {fixture.socketPath(), targets, backend};

    std::vector<mpfshost::ScatterEntry> entries;
    entries.reserve(mpfshost::kMaxPhysicalScatterEntries + 1);
    for (std::size_t index = 0; index <= mpfshost::kMaxPhysicalScatterEntries; ++index)
    {
        entries.push_back({0x1000, 8, mpfshost::kScatterEntryRead, {}});
    }

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PhysicalScatterRequest,
                5,
                mpfshost::encodePhysicalScatterRequest({0, std::move(entries)}));

    REQUIRE(receiveError(client, 5) == mpfshost::ErrorCode::MalformedPayload);
}

TEST_CASE("server rejects a physical scatter entry outside the partition", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x8000)});
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PhysicalScatterRequest,
                6,
                mpfshost::encodePhysicalScatterRequest({0, {{0x8000, 8, mpfshost::kScatterEntryRead, {}}}}));

    REQUIRE(receiveError(client, 6) == mpfshost::ErrorCode::InvalidRange);
}

TEST_CASE("server rejects a physical scatter for an unknown vm", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x8000)});
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PhysicalScatterRequest,
                7,
                mpfshost::encodePhysicalScatterRequest({3, {{0x1000, 8, mpfshost::kScatterEntryRead, {}}}}));

    REQUIRE(receiveError(client, 7) == mpfshost::ErrorCode::UnknownVm);
}

TEST_CASE("server rejects a physical scatter without the capability", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setHandlesBare(true);
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PhysicalScatterRequest,
                8,
                mpfshost::encodePhysicalScatterRequest({0, {{0x1000, 8, mpfshost::kScatterEntryRead, {}}}}));

    REQUIRE(receiveError(client, 8) == mpfshost::ErrorCode::CapabilityUnavailable);
}

TEST_CASE("server ends a failed physical scatter with one error frame", "[physical]")
{
    PhysicalFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({makeVm("physical", 0x8000)});
    backend.physicalState()->scatterFailure = "VMMDLL_VmMemReadScatter failed";
    PhysicalServer server {fixture.socketPath(), targets, backend};

    PhysicalClient client {fixture.socketPath()};
    bindSession(client, 1, "vm-alpha");
    client.send(mpfshost::MessageType::PhysicalScatterRequest,
                9,
                mpfshost::encodePhysicalScatterRequest({0, {{0x1000, 8, mpfshost::kScatterEntryRead, {}}}}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::PhysicalScatterEnd);
    REQUIRE(requestId == 9);

    const auto end = mpfshost::decodeScatterExecuteEnd(payload);
    REQUIRE(end.has_value());
    REQUIRE(end->status == 1);
    REQUIRE(end->error == "VMMDLL_VmMemReadScatter failed");
    REQUIRE(end->outcomes.empty());
}
