#include <catch2/catch.hpp>

#include "core/client/client.hpp"
#include "core/ipc/frame.hpp"
#include "core/ipc/message.hpp"
#include "core/server/server.hpp"
#include "support/fake_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
    using namespace std::chrono_literals;

    class DebugFixture
    {
    public:
        DebugFixture()
        {
            std::filesystem::create_directories(MPFSHOST_TEST_TMP_DIR);
            std::string pattern = (std::filesystem::path {MPFSHOST_TEST_TMP_DIR} / "debug-XXXXXX").string();
            REQUIRE(::mkdtemp(pattern.data()) != nullptr);
            _root       = pattern;
            _socketPath = _root / "host.sock";
            REQUIRE(_socketPath.string().size() < sizeof(sockaddr_un::sun_path));
            _targets.setTargets({
                mpfshost::VmTarget {"vm1", 42, _root / "qmp.sock"}
            });
        }

        ~DebugFixture()
        {
            std::error_code error;
            std::filesystem::remove_all(_root, error);
        }

        DebugFixture(const DebugFixture&)            = delete;
        DebugFixture& operator=(const DebugFixture&) = delete;

        [[nodiscard]] const std::filesystem::path& socketPath() const noexcept
        {
            return _socketPath;
        }

        [[nodiscard]] mpfshost::testing::FakeBackend& backend() noexcept
        {
            return _backend;
        }

        [[nodiscard]] mpfshost::testing::FakeTargetSource& targets() noexcept
        {
            return _targets;
        }

    private:
        std::filesystem::path               _root;
        std::filesystem::path               _socketPath;
        mpfshost::testing::FakeTargetSource _targets;
        mpfshost::testing::FakeBackend      _backend;
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
} // namespace

TEST_CASE("debug control request round trips and rejects unknown actions", "[core][debug][codec]")
{
    const auto encoded = mpfshost::encodeDebugControlRequest({mpfshost::DebugControlAction::Continue});
    const auto decoded = mpfshost::decodeDebugControlRequest(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->action == mpfshost::DebugControlAction::Continue);

    REQUIRE_FALSE(mpfshost::decodeDebugControlRequest({}).has_value());
    REQUIRE_FALSE(mpfshost::decodeDebugControlRequest(std::vector<std::byte> {static_cast<std::byte>(9)}).has_value());
    REQUIRE_FALSE(
        mpfshost::decodeDebugControlRequest(std::vector<std::byte> {std::byte {0}, std::byte {0}}).has_value());
}

TEST_CASE("debug status round trips through both response codecs", "[core][debug][codec]")
{
    mpfshost::DebugStatus status;
    status.runState   = mpfshost::DebugRunState::Paused;
    status.stubActive = true;
    status.hostPaused = true;
    status.vcpuCount  = 12;
    status.watchCount = 2;

    const auto state   = mpfshost::decodeDebugStateResponse(mpfshost::encodeDebugStateResponse(status));
    const auto control = mpfshost::decodeDebugControlResponse(mpfshost::encodeDebugControlResponse(status));
    REQUIRE(state.has_value());
    REQUIRE(control.has_value());
    REQUIRE(state->runState == mpfshost::DebugRunState::Paused);
    REQUIRE(state->stubActive);
    REQUIRE(state->hostPaused);
    REQUIRE(state->vcpuCount == 12);
    REQUIRE(state->watchCount == 2);
    REQUIRE(control->runState == mpfshost::DebugRunState::Paused);

    REQUIRE_FALSE(mpfshost::decodeDebugStateResponse({}).has_value());
}

TEST_CASE("debug requests report the fake status and control action", "[core][debug][session]")
{
    DebugFixture  fixture;
    RunningServer server {fixture.socketPath(), fixture.targets(), fixture.backend()};

    mpfshost::DebugStatus status;
    status.runState   = mpfshost::DebugRunState::Running;
    status.stubActive = false;
    status.hostPaused = false;
    status.vcpuCount  = 4;
    fixture.backend().setDebugStatus(status);

    std::string error;
    const auto  state = mpfshost::debugState(fixture.socketPath(), {"vm1"}, error);
    REQUIRE(state.has_value());
    REQUIRE(error.empty());
    REQUIRE(state->runState == mpfshost::DebugRunState::Running);
    REQUIRE(state->vcpuCount == 4);
    REQUIRE(fixture.backend().debugState()->stateCalls == 1);

    const auto stopped =
        mpfshost::debugControl(fixture.socketPath(), {"vm1"}, mpfshost::DebugControlAction::Stop, error);
    REQUIRE(stopped.has_value());
    REQUIRE(fixture.backend().debugState()->controlCalls == 1);
    REQUIRE(fixture.backend().debugState()->lastAction == mpfshost::DebugControlAction::Stop);
}

TEST_CASE("debug targets that are not bound are refused", "[core][debug][session]")
{
    DebugFixture  fixture;
    RunningServer server {fixture.socketPath(), fixture.targets(), fixture.backend()};

    std::string error;
    const auto  state = mpfshost::debugState(fixture.socketPath(), {"missing"}, error);
    REQUIRE_FALSE(state.has_value());
    REQUIRE(error.find("not available") != std::string::npos);
}

TEST_CASE("debug capabilities are listed", "[core][debug][session]")
{
    DebugFixture  fixture;
    RunningServer server {fixture.socketPath(), fixture.targets(), fixture.backend()};

    std::string error;
    const auto  capabilities = mpfshost::getCapabilities(fixture.socketPath(), error);
    REQUIRE(capabilities.has_value());

    const auto has = [&](mpfshost::MessageType type)
    {
        const auto value = static_cast<std::uint16_t>(type);
        return std::find(capabilities->requestTypes.begin(), capabilities->requestTypes.end(), value)
            != capabilities->requestTypes.end();
    };
    REQUIRE(has(mpfshost::MessageType::DebugStateRequest));
    REQUIRE(has(mpfshost::MessageType::DebugControlRequest));
}

TEST_CASE("a handle without the debug provider reports a capability error", "[core][debug][session]")
{
    DebugFixture fixture;
    fixture.backend().setHandlesBare(true);
    RunningServer server {fixture.socketPath(), fixture.targets(), fixture.backend()};

    std::string error;
    const auto  state = mpfshost::debugState(fixture.socketPath(), {"vm1"}, error);
    REQUIRE_FALSE(state.has_value());
    REQUIRE(error.find("cannot report debug state") != std::string::npos);
}

TEST_CASE("a provider failure is reported to the client", "[core][debug][session]")
{
    DebugFixture fixture;
    fixture.backend().failDebugControlWith("QMP control failed");
    RunningServer server {fixture.socketPath(), fixture.targets(), fixture.backend()};

    std::string error;
    const auto  stopped =
        mpfshost::debugControl(fixture.socketPath(), {"vm1"}, mpfshost::DebugControlAction::Stop, error);
    REQUIRE_FALSE(stopped.has_value());
    REQUIRE(error == "QMP control failed");
}
