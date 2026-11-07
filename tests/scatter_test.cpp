#include <catch2/catch.hpp>

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
#include <functional>
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
    class ScatterFixture
    {
    public:
        ScatterFixture()
        {
            std::filesystem::create_directories(MPFSHOST_TEST_TMP_DIR);
            std::string pattern = (std::filesystem::path {MPFSHOST_TEST_TMP_DIR} / "scatter-XXXXXX").string();
            REQUIRE(::mkdtemp(pattern.data()) != nullptr);
            _root       = pattern;
            _socketPath = _root / "host.sock";
            REQUIRE(_socketPath.string().size() < sizeof(sockaddr_un::sun_path));
        }

        ~ScatterFixture()
        {
            std::error_code error;
            std::filesystem::remove_all(_root, error);
        }

        ScatterFixture(const ScatterFixture&)            = delete;
        ScatterFixture& operator=(const ScatterFixture&) = delete;

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
            reset();
        }

        ClientSocket(const ClientSocket&)            = delete;
        ClientSocket& operator=(const ClientSocket&) = delete;

        void reset()
        {
            if (_fd >= 0)
            {
                ::close(_fd);
                _fd = -1;
            }
        }

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

    void bindClient(ClientSocket& client, std::uint64_t requestId, const std::string& targetName)
    {
        client.send(mpfshost::MessageType::BindRequest, requestId, mpfshost::encodeBindRequest({targetName}));

        mpfshost::MessageType  type;
        std::uint64_t          responseId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, responseId, payload));
        REQUIRE(type == mpfshost::MessageType::BindResponse);
    }

    std::uint32_t createScatter(ClientSocket& client, std::uint64_t requestId, std::uint32_t pid)
    {
        client.send(
            mpfshost::MessageType::ScatterCreateRequest, requestId, mpfshost::encodeScatterCreateRequest({pid}));

        mpfshost::MessageType  type;
        std::uint64_t          responseId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, responseId, payload));
        REQUIRE(type == mpfshost::MessageType::ScatterCreateResponse);
        const auto response = mpfshost::decodeScatterCreateResponse(payload);
        REQUIRE(response.has_value());
        return response->scatterId;
    }

    mpfshost::ErrorCode expectError(ClientSocket& client, std::uint64_t requestId)
    {
        mpfshost::MessageType  type;
        std::uint64_t          responseId {};
        std::vector<std::byte> payload;
        REQUIRE(client.receive(type, responseId, payload));
        REQUIRE(responseId == requestId);
        REQUIRE(type == mpfshost::MessageType::ErrorResponse);
        const auto error = mpfshost::decodeErrorResponse(payload);
        REQUIRE(error.has_value());
        return error->code;
    }

    struct ScatterStream
    {
        std::vector<std::vector<std::byte>>   entryData;
        std::vector<mpfshost::ScatterOutcome> outcomes;
        std::uint8_t                          status {};
        std::string                           error;
    };

    ScatterStream receiveScatterStream(ClientSocket&         client,
                                       std::uint64_t         requestId,
                                       mpfshost::MessageType dataType,
                                       mpfshost::MessageType endType)
    {
        ScatterStream result;
        while (true)
        {
            mpfshost::MessageType  type;
            std::uint64_t          responseId {};
            std::vector<std::byte> payload;
            REQUIRE(client.receive(type, responseId, payload));
            REQUIRE(responseId == requestId);
            if (type == endType)
            {
                const auto end = endType == mpfshost::MessageType::ScatterExecuteReadEnd
                                   ? mpfshost::decodeScatterExecuteReadEnd(payload)
                                   : mpfshost::decodeScatterExecuteEnd(payload);
                REQUIRE(end.has_value());
                result.status   = end->status;
                result.outcomes = end->outcomes;
                result.error    = end->error;
                return result;
            }

            REQUIRE(type == dataType);
            const auto data = dataType == mpfshost::MessageType::ScatterExecuteReadData
                                ? mpfshost::decodeScatterExecuteReadData(payload)
                                : mpfshost::decodeScatterExecuteData(payload);
            REQUIRE(data.has_value());
            if (result.entryData.size() <= data->entryIndex)
            {
                result.entryData.resize(data->entryIndex + 1);
            }
            result.entryData[data->entryIndex].insert(
                result.entryData[data->entryIndex].end(), data->data.begin(), data->data.end());
        }
    }
} // namespace

TEST_CASE("a scatter handle runs the full create, prepare, execute and close lifecycle", "[server][scatter]")
{
    ScatterFixture                      fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    const auto scatterId = createScatter(client, 2, 42);
    REQUIRE(scatterId >= 1);

    mpfshost::ScatterPrepareRequest prepare {scatterId, {}};
    prepare.entries.push_back({0x1000, 16, mpfshost::kScatterEntryRead, {}});
    prepare.entries.push_back({0x2000, 8, mpfshost::kScatterEntryRead, {}});
    client.send(mpfshost::MessageType::ScatterPrepareRequest, 3, mpfshost::encodeScatterPrepareRequest(prepare));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ScatterPrepareResponse);
    const auto prepared = mpfshost::decodeScatterPrepareResponse(payload);
    REQUIRE(prepared.has_value());
    REQUIRE(prepared->entryCount == 2);

    mpfshost::ScatterPrepareRequest more {scatterId, {}};
    more.entries.push_back({
        0x3000, 4, mpfshost::kScatterEntryWrite, {std::byte {1}, std::byte {2}, std::byte {3}, std::byte {4}}
    });
    client.send(mpfshost::MessageType::ScatterPrepareRequest, 4, mpfshost::encodeScatterPrepareRequest(more));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ScatterPrepareResponse);
    const auto preparedMore = mpfshost::decodeScatterPrepareResponse(payload);
    REQUIRE(preparedMore.has_value());
    REQUIRE(preparedMore->entryCount == 3);

    client.send(mpfshost::MessageType::ScatterExecuteRequest, 5, mpfshost::encodeScatterExecuteRequest({scatterId}));

    std::vector<std::vector<std::byte>>   entryData;
    std::vector<mpfshost::ScatterOutcome> outcomes;
    while (true)
    {
        REQUIRE(client.receive(type, requestId, payload));
        REQUIRE(requestId == 5);
        if (type == mpfshost::MessageType::ScatterExecuteEnd)
        {
            const auto end = mpfshost::decodeScatterExecuteEnd(payload);
            REQUIRE(end.has_value());
            REQUIRE(end->status == 0);
            outcomes = end->outcomes;
            break;
        }

        REQUIRE(type == mpfshost::MessageType::ScatterExecuteData);
        const auto data = mpfshost::decodeScatterExecuteData(payload);
        REQUIRE(data.has_value());
        if (entryData.size() <= data->entryIndex)
        {
            entryData.resize(data->entryIndex + 1);
        }
        entryData[data->entryIndex].insert(entryData[data->entryIndex].end(), data->data.begin(), data->data.end());
    }

    REQUIRE(outcomes.size() == 3);
    REQUIRE(outcomes[0].address == 0x1000);
    REQUIRE(outcomes[0].bytesRead == 16);
    REQUIRE(outcomes[0].ok == 1);
    REQUIRE(outcomes[1].address == 0x2000);
    REQUIRE(outcomes[1].bytesRead == 8);
    REQUIRE(outcomes[1].ok == 1);
    REQUIRE(outcomes[2].address == 0x3000);
    REQUIRE(outcomes[2].bytesRead == 0);
    REQUIRE(outcomes[2].ok == 1);

    REQUIRE(entryData.size() == 3);
    REQUIRE(entryData[0].size() == 16);
    REQUIRE(entryData[0][0] == static_cast<std::byte>(0x00));
    REQUIRE(entryData[0][15] == static_cast<std::byte>(0x0F));
    REQUIRE(entryData[1].size() == 8);
    REQUIRE(entryData[1][0] == static_cast<std::byte>(0x00));
    REQUIRE(entryData[2].empty());

    client.send(mpfshost::MessageType::ScatterCloseRequest, 6, mpfshost::encodeScatterCloseRequest({scatterId}));
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ScatterCloseResponse);
    const auto closed = mpfshost::decodeScatterCloseResponse(payload);
    REQUIRE(closed.has_value());
    REQUIRE(closed->scatterId == scatterId);

    client.send(mpfshost::MessageType::ScatterCloseRequest, 7, mpfshost::encodeScatterCloseRequest({scatterId}));
    REQUIRE(expectError(client, 7) == mpfshost::ErrorCode::UnknownScatter);

    const auto scatterState = backend.scatterState();
    REQUIRE(scatterState->creates == 1);
    REQUIRE(scatterState->entries.size() == 3);
    REQUIRE(scatterState->pid == 42);
}

TEST_CASE("a scatter handle re-reads only its prepared read ranges", "[server][scatter]")
{
    ScatterFixture                      fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    const auto scatterId = createScatter(client, 2, 7);

    mpfshost::ScatterPrepareRequest prepare {scatterId, {}};
    prepare.entries.push_back({0x1000, 8, mpfshost::kScatterEntryRead, {}});
    prepare.entries.push_back({
        0x2000, 4, mpfshost::kScatterEntryWrite, {std::byte {1}, std::byte {2}, std::byte {3}, std::byte {4}}
    });
    client.send(mpfshost::MessageType::ScatterPrepareRequest, 3, mpfshost::encodeScatterPrepareRequest(prepare));

    mpfshost::MessageType  type;
    std::uint64_t          responseId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, responseId, payload));
    REQUIRE(type == mpfshost::MessageType::ScatterPrepareResponse);

    client.send(
        mpfshost::MessageType::ScatterExecuteReadRequest, 4, mpfshost::encodeScatterExecuteReadRequest({scatterId}));
    const auto stream = receiveScatterStream(
        client, 4, mpfshost::MessageType::ScatterExecuteReadData, mpfshost::MessageType::ScatterExecuteReadEnd);
    REQUIRE(stream.status == 0);
    REQUIRE(stream.outcomes.size() == 1);
    REQUIRE(stream.outcomes[0].address == 0x1000);
    REQUIRE(stream.outcomes[0].bytesRead == 8);
    REQUIRE(stream.outcomes[0].ok == 1);
    REQUIRE(stream.entryData.size() == 1);
    REQUIRE(stream.entryData[0].size() == 8);

    const auto scatterState = backend.scatterState();
    REQUIRE(scatterState->executeReads == 1);
}

TEST_CASE("a scatter handle can be cleared and reused", "[server][scatter]")
{
    ScatterFixture                      fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    const auto scatterId = createScatter(client, 2, 7);

    mpfshost::ScatterPrepareRequest prepare {scatterId, {}};
    prepare.entries.push_back({0x1000, 8, mpfshost::kScatterEntryRead, {}});
    prepare.entries.push_back({0x2000, 8, mpfshost::kScatterEntryRead, {}});
    client.send(mpfshost::MessageType::ScatterPrepareRequest, 3, mpfshost::encodeScatterPrepareRequest(prepare));

    mpfshost::MessageType  type;
    std::uint64_t          responseId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, responseId, payload));
    REQUIRE(type == mpfshost::MessageType::ScatterPrepareResponse);
    const auto prepared = mpfshost::decodeScatterPrepareResponse(payload);
    REQUIRE(prepared.has_value());
    REQUIRE(prepared->entryCount == 2);

    client.send(mpfshost::MessageType::ScatterClearRequest, 4, mpfshost::encodeScatterClearRequest({scatterId}));
    REQUIRE(client.receive(type, responseId, payload));
    REQUIRE(type == mpfshost::MessageType::ScatterClearResponse);
    const auto cleared = mpfshost::decodeScatterClearResponse(payload);
    REQUIRE(cleared.has_value());
    REQUIRE(cleared->scatterId == scatterId);

    mpfshost::ScatterPrepareRequest again {scatterId, {}};
    again.entries.push_back({0x3000, 4, mpfshost::kScatterEntryRead, {}});
    client.send(mpfshost::MessageType::ScatterPrepareRequest, 5, mpfshost::encodeScatterPrepareRequest(again));
    REQUIRE(client.receive(type, responseId, payload));
    REQUIRE(type == mpfshost::MessageType::ScatterPrepareResponse);
    const auto preparedAgain = mpfshost::decodeScatterPrepareResponse(payload);
    REQUIRE(preparedAgain.has_value());
    REQUIRE(preparedAgain->entryCount == 1);

    const auto scatterState = backend.scatterState();
    REQUIRE(scatterState->clears == 1);
    REQUIRE(scatterState->entries.size() == 1);
    REQUIRE(scatterState->entries[0].address == 0x3000);

    client.send(mpfshost::MessageType::ScatterClearRequest, 6, mpfshost::encodeScatterClearRequest({99}));
    REQUIRE(expectError(client, 6) == mpfshost::ErrorCode::UnknownScatter);

    backend.scatterState()->clearFailure = "clear backend failure";
    client.send(mpfshost::MessageType::ScatterClearRequest, 7, mpfshost::encodeScatterClearRequest({scatterId}));
    REQUIRE(expectError(client, 7) == mpfshost::ErrorCode::Internal);
}

TEST_CASE("a failing scatter read still closes the stream", "[server][scatter]")
{
    ScatterFixture                      fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    const auto scatterId                       = createScatter(client, 2, 7);
    backend.scatterState()->executeReadFailure = "scatter read backend failure";

    client.send(
        mpfshost::MessageType::ScatterExecuteReadRequest, 3, mpfshost::encodeScatterExecuteReadRequest({scatterId}));
    const auto stream = receiveScatterStream(
        client, 3, mpfshost::MessageType::ScatterExecuteReadData, mpfshost::MessageType::ScatterExecuteReadEnd);
    REQUIRE(stream.status == 1);
    REQUIRE(stream.outcomes.empty());
    REQUIRE(stream.error == "scatter read backend failure");
}

TEST_CASE("scatter handles are session scoped and capped", "[server][scatter]")
{
    ScatterFixture                      fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    std::vector<std::uint32_t> ids;
    for (std::uint64_t index = 0; index < mpfshost::kMaxScatterHandlesPerSession; ++index)
    {
        ids.push_back(createScatter(client, index + 2, 7));
    }
    REQUIRE(ids.size() == mpfshost::kMaxScatterHandlesPerSession);

    client.send(mpfshost::MessageType::ScatterCreateRequest, 100, mpfshost::encodeScatterCreateRequest({7}));
    REQUIRE(expectError(client, 100) == mpfshost::ErrorCode::ScatterLimitReached);

    ClientSocket other {fixture.socketPath()};
    bindClient(other, 1, "vm-alpha");

    for (const auto id : ids)
    {
        other.send(mpfshost::MessageType::ScatterPrepareRequest, 200, mpfshost::encodeScatterPrepareRequest({id, {}}));
        REQUIRE(expectError(other, 200) == mpfshost::ErrorCode::UnknownScatter);
    }
}

TEST_CASE("scatter prepare validates entries and reports unknown ids", "[server][scatter]")
{
    ScatterFixture                      fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::ScatterPrepareRequest, 2, mpfshost::encodeScatterPrepareRequest({99, {}}));
    REQUIRE(expectError(client, 2) == mpfshost::ErrorCode::UnknownScatter);

    const auto scatterId = createScatter(client, 3, 7);

    mpfshost::ScatterPrepareRequest mismatched {scatterId, {}};
    mismatched.entries.push_back({0x1000, 8, mpfshost::kScatterEntryWrite, {std::byte {1}}});
    client.send(mpfshost::MessageType::ScatterPrepareRequest, 4, mpfshost::encodeScatterPrepareRequest(mismatched));
    REQUIRE(expectError(client, 4) == mpfshost::ErrorCode::InvalidRange);

    mpfshost::ScatterPrepareRequest badKind {scatterId, {}};
    badKind.entries.push_back({0x1000, 8, 9, {}});
    client.send(mpfshost::MessageType::ScatterPrepareRequest, 5, mpfshost::encodeScatterPrepareRequest(badKind));
    REQUIRE(expectError(client, 5) == mpfshost::ErrorCode::InvalidRange);
}

TEST_CASE("a failing scatter execution still closes the stream", "[server][scatter]")
{
    ScatterFixture                      fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    const auto scatterId                   = createScatter(client, 2, 7);
    backend.scatterState()->executeFailure = "scatter backend failure";

    client.send(mpfshost::MessageType::ScatterExecuteRequest, 3, mpfshost::encodeScatterExecuteRequest({scatterId}));

    mpfshost::MessageType  type;
    std::uint64_t          requestId {};
    std::vector<std::byte> payload;
    REQUIRE(client.receive(type, requestId, payload));
    REQUIRE(type == mpfshost::MessageType::ScatterExecuteEnd);
    const auto end = mpfshost::decodeScatterExecuteEnd(payload);
    REQUIRE(end.has_value());
    REQUIRE(end->status == 1);
    REQUIRE(end->outcomes.empty());
    REQUIRE(end->error == "scatter backend failure");
}

TEST_CASE("scatter handles are released when the connection ends", "[server][scatter]")
{
    ScatterFixture                      fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    {
        ClientSocket client {fixture.socketPath()};
        bindClient(client, 1, "vm-alpha");
        createScatter(client, 2, 7);
        createScatter(client, 3, 7);

        const auto state = backend.scatterState();
        REQUIRE(waitFor(
            [&state]
            {
                return state->liveHandles.load() == 2;
            }));
    }

    const auto state = backend.scatterState();
    REQUIRE(waitFor(
        [&state]
        {
            return state->liveHandles.load() == 0;
        }));
}

TEST_CASE("scatter creation reports CapabilityUnavailable without a scatter-capable handle", "[server][scatter]")
{
    ScatterFixture                      fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setHandlesBare(true);
    RunningServer server {fixture.socketPath(), targets, backend};

    ClientSocket client {fixture.socketPath()};
    bindClient(client, 1, "vm-alpha");

    client.send(mpfshost::MessageType::ScatterCreateRequest, 2, mpfshost::encodeScatterCreateRequest({7}));
    REQUIRE(expectError(client, 2) == mpfshost::ErrorCode::CapabilityUnavailable);
}
