#include <catch2/catch.hpp>

#include "core/client/client.hpp"
#include "core/client/client_transport.hpp"
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
    status.runState    = mpfshost::DebugRunState::Paused;
    status.stubActive  = true;
    status.hostPaused  = true;
    status.vcpuCount   = 12;
    status.watchCount  = 2;
    status.stubOwned   = true;
    status.foreignStub = false;

    const auto state   = mpfshost::decodeDebugStateResponse(mpfshost::encodeDebugStateResponse(status));
    const auto control = mpfshost::decodeDebugControlResponse(mpfshost::encodeDebugControlResponse(status));
    REQUIRE(state.has_value());
    REQUIRE(control.has_value());
    REQUIRE(state->runState == mpfshost::DebugRunState::Paused);
    REQUIRE(state->stubActive);
    REQUIRE(state->hostPaused);
    REQUIRE(state->vcpuCount == 12);
    REQUIRE(state->watchCount == 2);
    REQUIRE(state->stubOwned);
    REQUIRE_FALSE(state->foreignStub);
    REQUIRE(control->runState == mpfshost::DebugRunState::Paused);

    status.stubOwned   = false;
    status.foreignStub = true;
    const auto foreign = mpfshost::decodeDebugStateResponse(mpfshost::encodeDebugStateResponse(status));
    REQUIRE(foreign.has_value());
    REQUIRE_FALSE(foreign->stubOwned);
    REQUIRE(foreign->foreignStub);

    REQUIRE_FALSE(mpfshost::decodeDebugStateResponse({}).has_value());
}

TEST_CASE("debug detach request and response round trip", "[core][debug][codec]")
{
    REQUIRE(mpfshost::decodeDebugDetachRequest(mpfshost::encodeDebugDetachRequest({})).has_value());
    REQUIRE_FALSE(mpfshost::decodeDebugDetachRequest(std::vector<std::byte> {static_cast<std::byte>(1)}).has_value());

    mpfshost::DebugDetachResponse response;
    response.status.runState   = mpfshost::DebugRunState::Running;
    response.status.stubActive = false;
    response.status.vcpuCount  = 12;
    response.stubStopped       = true;
    response.foreignStub       = true;
    response.guestResumed      = false;

    const auto decoded = mpfshost::decodeDebugDetachResponse(mpfshost::encodeDebugDetachResponse(response));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->status.runState == mpfshost::DebugRunState::Running);
    REQUIRE_FALSE(decoded->status.stubActive);
    REQUIRE(decoded->status.vcpuCount == 12);
    REQUIRE(decoded->stubStopped);
    REQUIRE(decoded->foreignStub);
    REQUIRE_FALSE(decoded->guestResumed);

    REQUIRE_FALSE(mpfshost::decodeDebugDetachResponse({}).has_value());
}

TEST_CASE("debug register requests and responses round trip", "[core][debug][codec]")
{
    const auto request = mpfshost::decodeDebugRegistersRequest(mpfshost::encodeDebugRegistersRequest({3}));
    REQUIRE(request.has_value());
    REQUIRE(request->vcpu == 3);

    mpfshost::DebugRegisterRecord record;
    record.vcpu     = 2;
    record.threadId = "p01.03";
    record.rip      = 0x1234;
    record.cr3      = 0xabc000;
    record.rax      = 0x55;
    record.r15      = 0x66;

    mpfshost::DebugRegistersResponse response;
    response.records.push_back(record);
    const auto decoded = mpfshost::decodeDebugRegistersResponse(mpfshost::encodeDebugRegistersResponse(response));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->records.size() == 1);
    REQUIRE(decoded->records[0].vcpu == 2);
    REQUIRE(decoded->records[0].threadId == "p01.03");
    REQUIRE(decoded->records[0].rip == 0x1234);
    REQUIRE(decoded->records[0].cr3 == 0xabc000);
    REQUIRE(decoded->records[0].rax == 0x55);
    REQUIRE(decoded->records[0].r15 == 0x66);

    REQUIRE_FALSE(mpfshost::decodeDebugRegistersResponse({}).has_value());
    REQUIRE_FALSE(
        mpfshost::decodeDebugRegistersRequest(std::vector<std::byte> {static_cast<std::byte>(0)}).has_value());
}

TEST_CASE("debug watch requests, hits and ends round trip", "[core][debug][codec]")
{
    const auto request = mpfshost::decodeDebugWatchRequest(
        mpfshost::encodeDebugWatchRequest({7, 0x12345678, 4, mpfshost::DebugWatchAccess::Write, 3, 1500}));
    REQUIRE(request.has_value());
    REQUIRE(request->pid == 7);
    REQUIRE(request->address == 0x12345678);
    REQUIRE(request->width == 4);
    REQUIRE(request->maxHits == 3);
    REQUIRE(request->timeoutMs == 1500);

    mpfshost::DebugWatchData hit;
    hit.hitIndex        = 1;
    hit.vcpu            = 8;
    hit.threadId        = "p01.09";
    hit.rip             = 0x7ff84522b1f1;
    hit.rsp             = 0x1234;
    hit.cr3             = 0x200123;
    hit.address         = 0x5000;
    hit.oldValue        = 0xAA;
    hit.newValue        = 0xBB;
    hit.bytesBefore     = {std::byte {0x90}, std::byte {0xC3}};
    hit.module          = "msvcrt.dll";
    hit.moduleOffset    = 0x7b1f1;
    hit.exportFound     = true;
    hit.exportAddress   = 0x7ff84522b180;
    hit.exportOffset    = 0x71;
    hit.exportName      = "memcpy";
    hit.pdbFound        = true;
    hit.pdbSymbol       = "memcpy";
    hit.pdbDisplacement = 4;

    const auto decodedHit = mpfshost::decodeDebugWatchData(mpfshost::encodeDebugWatchData(hit));
    REQUIRE(decodedHit.has_value());
    REQUIRE(decodedHit->hitIndex == 1);
    REQUIRE(decodedHit->vcpu == 8);
    REQUIRE(decodedHit->threadId == "p01.09");
    REQUIRE(decodedHit->rip == hit.rip);
    REQUIRE(decodedHit->cr3 == hit.cr3);
    REQUIRE(decodedHit->oldValue == 0xAA);
    REQUIRE(decodedHit->newValue == 0xBB);
    REQUIRE(decodedHit->bytesBefore.size() == 2);
    REQUIRE(decodedHit->module == "msvcrt.dll");
    REQUIRE(decodedHit->exportFound);
    REQUIRE(decodedHit->exportName == "memcpy");
    REQUIRE(decodedHit->pdbFound);
    REQUIRE(decodedHit->pdbDisplacement == 4);

    const auto decodedEnd = mpfshost::decodeDebugWatchEnd(
        mpfshost::encodeDebugWatchEnd({mpfshost::DebugWatchStatus::Timeout, 2, 1, "timed out"}));
    REQUIRE(decodedEnd.has_value());
    REQUIRE(decodedEnd->status == mpfshost::DebugWatchStatus::Timeout);
    REQUIRE(decodedEnd->hitCount == 2);
    REQUIRE(decodedEnd->skippedCount == 1);
    REQUIRE(decodedEnd->error == "timed out");

    REQUIRE_FALSE(mpfshost::decodeDebugWatchRequest({}).has_value());
    REQUIRE_FALSE(mpfshost::decodeDebugWatchData({}).has_value());
    REQUIRE_FALSE(mpfshost::decodeDebugWatchEnd({}).has_value());
}

TEST_CASE("debug breakpoint and step messages round trip", "[core][debug][codec]")
{
    const auto request = mpfshost::decodeDebugBreakpointRequest(
        mpfshost::encodeDebugBreakpointRequest({9, 0x7ff84522b1f1ull, mpfshost::DebugBreakpointKind::Hardware, 2500}));
    REQUIRE(request.has_value());
    REQUIRE(request->pid == 9);
    REQUIRE(request->address == 0x7ff84522b1f1ull);
    REQUIRE(request->kind == mpfshost::DebugBreakpointKind::Hardware);
    REQUIRE(request->timeoutMs == 2500);

    mpfshost::DebugBreakpointResponse response;
    response.status          = mpfshost::DebugBreakpointStatus::Hit;
    response.skippedCount    = 1;
    response.error           = "none";
    response.vcpu            = 3;
    response.threadId        = "p01.04";
    response.rip             = 0x7ff84522b1f1ull;
    response.rsp             = 0x1234;
    response.cr3             = 0x200123;
    response.address         = 0x7ff84522b1f1ull;
    response.bytesAtRip      = {std::byte {0x90}, std::byte {0xC3}};
    response.module          = "msvcrt.dll";
    response.moduleOffset    = 0x7b1f1;
    response.exportFound     = true;
    response.exportAddress   = 0x7ff84522b180;
    response.exportOffset    = 0x71;
    response.exportName      = "memcpy";
    response.pdbFound        = true;
    response.pdbSymbol       = "memcpy";
    response.pdbDisplacement = 4;

    const auto decoded = mpfshost::decodeDebugBreakpointResponse(mpfshost::encodeDebugBreakpointResponse(response));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->status == mpfshost::DebugBreakpointStatus::Hit);
    REQUIRE(decoded->skippedCount == 1);
    REQUIRE(decoded->threadId == "p01.04");
    REQUIRE(decoded->rip == response.rip);
    REQUIRE(decoded->bytesAtRip.size() == 2);
    REQUIRE(decoded->module == "msvcrt.dll");
    REQUIRE(decoded->exportFound);
    REQUIRE(decoded->pdbFound);

    const auto stepRequest = mpfshost::decodeDebugStepRequest(mpfshost::encodeDebugStepRequest({2}));
    REQUIRE(stepRequest.has_value());
    REQUIRE(stepRequest->vcpu == 2);

    mpfshost::DebugStepResponse step;
    step.record.vcpu       = 2;
    step.record.threadId   = "p01.03";
    step.record.rip        = 0x1234;
    step.record.cr3        = 0x200123;
    step.record.r15        = 0x55;
    const auto decodedStep = mpfshost::decodeDebugStepResponse(mpfshost::encodeDebugStepResponse(step));
    REQUIRE(decodedStep.has_value());
    REQUIRE(decodedStep->record.vcpu == 2);
    REQUIRE(decodedStep->record.rip == 0x1234);
    REQUIRE(decodedStep->record.r15 == 0x55);

    REQUIRE_FALSE(mpfshost::decodeDebugBreakpointRequest({}).has_value());
    REQUIRE_FALSE(mpfshost::decodeDebugBreakpointResponse({}).has_value());
    REQUIRE_FALSE(mpfshost::decodeDebugStepRequest({}).has_value());
    REQUIRE_FALSE(mpfshost::decodeDebugStepResponse({}).has_value());
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
    REQUIRE(has(mpfshost::MessageType::DebugWatchRequest));
    REQUIRE(has(mpfshost::MessageType::DebugBreakpointRequest));
    REQUIRE(has(mpfshost::MessageType::DebugStepRequest));
    REQUIRE(has(mpfshost::MessageType::DebugDetachRequest));
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

TEST_CASE("a probe failure is reported to the client", "[core][debug][session]")
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

TEST_CASE("an unreachable debug channel is reported as a capability error", "[core][debug][session]")
{
    DebugFixture fixture;
    fixture.backend().failDebugControlWith("the QMP socket is not available");
    fixture.backend().debugState()->failureUnavailable = true;
    RunningServer server {fixture.socketPath(), fixture.targets(), fixture.backend()};

    std::string error;
    const auto  stopped =
        mpfshost::debugControl(fixture.socketPath(), {"vm1"}, mpfshost::DebugControlAction::Stop, error);
    REQUIRE_FALSE(stopped.has_value());
    REQUIRE(error.find("not available") != std::string::npos);

    const auto connection = mpfshost::detail::connectAndBind(fixture.socketPath(), "vm1", error);
    REQUIRE(connection.has_value());
    const auto response =
        mpfshost::detail::exchange(connection->fd(),
                                   mpfshost::MessageType::DebugControlRequest,
                                   mpfshost::encodeDebugControlRequest({mpfshost::DebugControlAction::Stop}),
                                   error);
    REQUIRE(response.has_value());
    REQUIRE(response->type == mpfshost::MessageType::ErrorResponse);
    const auto decoded = mpfshost::decodeErrorResponse(response->payload);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->code == mpfshost::ErrorCode::CapabilityUnavailable);
}

TEST_CASE("debug registers are delivered through the fake backend", "[core][debug][session]")
{
    DebugFixture                  fixture;
    mpfshost::DebugRegisterRecord record;
    record.vcpu     = 0;
    record.threadId = "p01.01";
    record.rip      = 0x1000;
    record.cr3      = 0x2000;
    fixture.backend().setDebugRegisters({record});
    RunningServer server {fixture.socketPath(), fixture.targets(), fixture.backend()};

    std::string error;
    const auto  records = mpfshost::debugRegisters(fixture.socketPath(), {"vm1", mpfshost::kDebugAllVcpus}, error);
    REQUIRE(records.has_value());
    REQUIRE(records->size() == 1);
    REQUIRE((*records)[0].rip == 0x1000);
    REQUIRE((*records)[0].cr3 == 0x2000);
    REQUIRE(fixture.backend().debugState()->registersCalls == 1);
    REQUIRE(fixture.backend().debugState()->lastRegistersRequest.has_value());
    REQUIRE(fixture.backend().debugState()->lastRegistersRequest->vcpu == mpfshost::kDebugAllVcpus);

    const auto single = mpfshost::debugRegisters(fixture.socketPath(), {"vm1", 0}, error);
    REQUIRE(single.has_value());
    REQUIRE(fixture.backend().debugState()->lastRegistersRequest->vcpu == 0);
}

TEST_CASE("an invalid debug state is reported as InvalidState", "[core][debug][session]")
{
    DebugFixture fixture;
    fixture.backend().failDebugRegistersWith("the guest must be paused before its registers can be read", true);
    RunningServer server {fixture.socketPath(), fixture.targets(), fixture.backend()};

    std::string error;
    const auto  records = mpfshost::debugRegisters(fixture.socketPath(), {"vm1"}, error);
    REQUIRE_FALSE(records.has_value());
    REQUIRE(error.find("must be paused") != std::string::npos);

    const auto connection = mpfshost::detail::connectAndBind(fixture.socketPath(), "vm1", error);
    REQUIRE(connection.has_value());
    const auto response = mpfshost::detail::exchange(connection->fd(),
                                                     mpfshost::MessageType::DebugRegistersRequest,
                                                     mpfshost::encodeDebugRegistersRequest({}),
                                                     error);
    REQUIRE(response.has_value());
    REQUIRE(response->type == mpfshost::MessageType::ErrorResponse);
    const auto decoded = mpfshost::decodeErrorResponse(response->payload);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->code == mpfshost::ErrorCode::InvalidState);
}

TEST_CASE("a handle without the debug provider cannot read registers", "[core][debug][session]")
{
    DebugFixture fixture;
    fixture.backend().setHandlesBare(true);
    RunningServer server {fixture.socketPath(), fixture.targets(), fixture.backend()};

    std::string error;
    const auto  records = mpfshost::debugRegisters(fixture.socketPath(), {"vm1"}, error);
    REQUIRE_FALSE(records.has_value());
    REQUIRE(error.find("cannot read debug registers") != std::string::npos);
}

TEST_CASE("debug watch hits stream through the fake backend", "[core][debug][session]")
{
    DebugFixture fixture;

    mpfshost::DebugWatchData hit;
    hit.hitIndex = 0;
    hit.vcpu     = 2;
    hit.threadId = "p01.03";
    hit.rip      = 0x1000;
    hit.address  = 0x2000;
    hit.oldValue = 1;
    hit.newValue = 2;
    hit.module   = "ntdll.dll";
    mpfshost::DebugWatchEnd end;
    end.status   = mpfshost::DebugWatchStatus::Completed;
    end.hitCount = 1;
    fixture.backend().setDebugWatch({hit}, end);
    RunningServer server {fixture.socketPath(), fixture.targets(), fixture.backend()};

    std::vector<mpfshost::DebugWatchData> received;
    std::string                           error;
    const auto                            result = mpfshost::debugWatch(
        fixture.socketPath(),
        {"vm1", 42, 0x2000, 4, mpfshost::DebugWatchAccess::Write, 2, 5000},
        [&received](const mpfshost::DebugWatchData& data)
        {
            received.push_back(data);
        },
        error);
    REQUIRE(result.has_value());
    REQUIRE(error.empty());
    REQUIRE(result->status == mpfshost::DebugWatchStatus::Completed);
    REQUIRE(result->hitCount == 1);
    REQUIRE(received.size() == 1);
    REQUIRE(received[0].rip == 0x1000);
    REQUIRE(received[0].module == "ntdll.dll");
    REQUIRE(fixture.backend().debugState()->watchCalls == 1);
    REQUIRE(fixture.backend().debugState()->lastWatchRequest.has_value());
    REQUIRE(fixture.backend().debugState()->lastWatchRequest->pid == 42);
    REQUIRE(fixture.backend().debugState()->lastWatchRequest->maxHits == 2);
}

TEST_CASE("debug breakpoints and single steps are delivered through the fake backend", "[core][debug][session]")
{
    DebugFixture fixture;

    mpfshost::DebugBreakpointResponse response;
    response.status   = mpfshost::DebugBreakpointStatus::Hit;
    response.vcpu     = 1;
    response.threadId = "p01.02";
    response.rip      = 0x1000;
    response.address  = 0x1000;
    fixture.backend().setDebugBreakpoint(response);

    mpfshost::DebugStepResponse step;
    step.record.vcpu     = 1;
    step.record.threadId = "p01.02";
    step.record.rip      = 0x1004;
    fixture.backend().setDebugStep(step);

    RunningServer server {fixture.socketPath(), fixture.targets(), fixture.backend()};

    std::string error;
    const auto  breakpoint = mpfshost::debugBreakpoint(
        fixture.socketPath(), {"vm1", 42, 0x1000, mpfshost::DebugBreakpointKind::Hardware, 5000}, error);
    REQUIRE(breakpoint.has_value());
    REQUIRE(breakpoint->status == mpfshost::DebugBreakpointStatus::Hit);
    REQUIRE(fixture.backend().debugState()->breakpointCalls == 1);
    REQUIRE(fixture.backend().debugState()->lastBreakpointRequest.has_value());
    REQUIRE(fixture.backend().debugState()->lastBreakpointRequest->kind == mpfshost::DebugBreakpointKind::Hardware);

    const auto singleStep = mpfshost::debugStep(fixture.socketPath(), {"vm1", 1}, error);
    REQUIRE(singleStep.has_value());
    REQUIRE(singleStep->record.rip == 0x1004);
    REQUIRE(fixture.backend().debugState()->stepCalls == 1);
    REQUIRE(fixture.backend().debugState()->lastStepRequest.has_value());
    REQUIRE(fixture.backend().debugState()->lastStepRequest->vcpu == 1);
}

TEST_CASE("debug detach is delivered through the fake backend", "[core][debug][session]")
{
    DebugFixture                  fixture;
    mpfshost::DebugDetachResponse response;
    response.status.runState   = mpfshost::DebugRunState::Running;
    response.status.stubActive = false;
    response.stubStopped       = true;
    response.foreignStub       = true;
    fixture.backend().setDebugDetach(response);
    RunningServer server {fixture.socketPath(), fixture.targets(), fixture.backend()};

    std::string error;
    const auto  detached = mpfshost::debugDetach(fixture.socketPath(), {"vm1"}, error);
    REQUIRE(detached.has_value());
    REQUIRE(detached->stubStopped);
    REQUIRE(detached->foreignStub);
    REQUIRE(detached->status.runState == mpfshost::DebugRunState::Running);
    REQUIRE(fixture.backend().debugState()->detachCalls == 1);
}

TEST_CASE("a concurrent debug detach is reported as Busy", "[core][debug][session]")
{
    DebugFixture fixture;
    fixture.backend().failDebugDetachWith("another debug operation is in progress", true);
    RunningServer server {fixture.socketPath(), fixture.targets(), fixture.backend()};

    std::string error;
    const auto  detached = mpfshost::debugDetach(fixture.socketPath(), {"vm1"}, error);
    REQUIRE_FALSE(detached.has_value());
    REQUIRE(error.find("another debug operation") != std::string::npos);
}

TEST_CASE("a handle without the debug provider cannot detach the debugger", "[core][debug][session]")
{
    DebugFixture fixture;
    fixture.backend().setHandlesBare(true);
    RunningServer server {fixture.socketPath(), fixture.targets(), fixture.backend()};

    std::string error;
    const auto  detached = mpfshost::debugDetach(fixture.socketPath(), {"vm1"}, error);
    REQUIRE_FALSE(detached.has_value());
    REQUIRE(error.find("cannot detach the debugger") != std::string::npos);
}

TEST_CASE("a running guest is reported as an invalid state for a single step", "[core][debug][session]")
{
    DebugFixture fixture;
    fixture.backend().failDebugStepWith("the guest must be paused before it can be single-stepped", true);
    RunningServer server {fixture.socketPath(), fixture.targets(), fixture.backend()};

    std::string error;
    const auto  response = mpfshost::debugStep(fixture.socketPath(), {"vm1", 0}, error);
    REQUIRE_FALSE(response.has_value());
    REQUIRE(error.find("must be paused") != std::string::npos);
}

TEST_CASE("a handle without the debug provider cannot run a watch", "[core][debug][session]")
{
    DebugFixture fixture;
    fixture.backend().setHandlesBare(true);
    RunningServer server {fixture.socketPath(), fixture.targets(), fixture.backend()};

    std::string error;
    const auto  result = mpfshost::debugWatch(
        fixture.socketPath(), {"vm1", 42, 0x2000, 4, mpfshost::DebugWatchAccess::Write, 1, 5000}, {}, error);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(error.find("cannot run a debug watch") != std::string::npos);
}

TEST_CASE("a concurrent debug operation is reported as Busy", "[core][debug][session]")
{
    DebugFixture fixture;
    fixture.backend().failDebugWatchWith("another debug operation is in progress", true);
    RunningServer server {fixture.socketPath(), fixture.targets(), fixture.backend()};

    std::string error;
    const auto  result = mpfshost::debugWatch(
        fixture.socketPath(), {"vm1", 42, 0x2000, 4, mpfshost::DebugWatchAccess::Write, 1, 5000}, {}, error);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(error.find("another debug operation") != std::string::npos);

    const auto connection = mpfshost::detail::connectAndBind(fixture.socketPath(), "vm1", error);
    REQUIRE(connection.has_value());
    const auto response = mpfshost::detail::exchange(
        connection->fd(),
        mpfshost::MessageType::DebugWatchRequest,
        mpfshost::encodeDebugWatchRequest({42, 0x2000, 4, mpfshost::DebugWatchAccess::Write, 1, 5000}),
        error);
    REQUIRE(response.has_value());
    REQUIRE(response->type == mpfshost::MessageType::ErrorResponse);
    const auto decoded = mpfshost::decodeErrorResponse(response->payload);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->code == mpfshost::ErrorCode::Busy);
}
