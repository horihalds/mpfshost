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
    status.runState     = mpfshost::DebugRunState::Paused;
    status.stubActive   = true;
    status.hostPaused   = true;
    status.vcpuCount    = 12;
    status.watchCount   = 2;
    status.stubOwned    = true;
    status.foreignStub  = false;
    status.architecture = "x86_64";

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
    REQUIRE(state->architecture == "x86_64");
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
    response.status.runState     = mpfshost::DebugRunState::Running;
    response.status.stubActive   = false;
    response.status.vcpuCount    = 12;
    response.status.architecture = "x86_64";
    response.stubStopped         = true;
    response.foreignStub         = true;
    response.guestResumed        = false;
    response.detached            = true;

    const auto decoded = mpfshost::decodeDebugDetachResponse(mpfshost::encodeDebugDetachResponse(response));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->status.runState == mpfshost::DebugRunState::Running);
    REQUIRE_FALSE(decoded->status.stubActive);
    REQUIRE(decoded->status.vcpuCount == 12);
    REQUIRE(decoded->status.architecture == "x86_64");
    REQUIRE(decoded->stubStopped);
    REQUIRE(decoded->foreignStub);
    REQUIRE_FALSE(decoded->guestResumed);
    REQUIRE(decoded->detached);

    REQUIRE_FALSE(mpfshost::decodeDebugDetachResponse({}).has_value());
}

TEST_CASE("debug attach request and response round trip", "[core][debug][codec]")
{
    REQUIRE(mpfshost::decodeDebugAttachRequest(mpfshost::encodeDebugAttachRequest({})).has_value());
    REQUIRE_FALSE(mpfshost::decodeDebugAttachRequest(std::vector<std::byte> {static_cast<std::byte>(1)}).has_value());

    mpfshost::DebugAttachResponse response;
    response.status.runState     = mpfshost::DebugRunState::Paused;
    response.status.stubActive   = true;
    response.status.stubOwned    = true;
    response.status.vcpuCount    = 12;
    response.status.architecture = "x86_64";
    response.stubMode            = "unix";
    response.guestPaused         = true;

    const auto decoded = mpfshost::decodeDebugAttachResponse(mpfshost::encodeDebugAttachResponse(response));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->status.runState == mpfshost::DebugRunState::Paused);
    REQUIRE(decoded->status.stubActive);
    REQUIRE(decoded->status.stubOwned);
    REQUIRE(decoded->status.vcpuCount == 12);
    REQUIRE(decoded->status.architecture == "x86_64");
    REQUIRE(decoded->stubMode == "unix");
    REQUIRE(decoded->guestPaused);

    auto invalidMode     = response;
    invalidMode.stubMode = "serial";
    REQUIRE_FALSE(mpfshost::decodeDebugAttachResponse(mpfshost::encodeDebugAttachResponse(invalidMode)).has_value());
    REQUIRE_FALSE(mpfshost::decodeDebugAttachResponse({}).has_value());
}

TEST_CASE("debug register requests and responses round trip", "[core][debug][codec]")
{
    const auto request = mpfshost::decodeDebugRegistersRequest(mpfshost::encodeDebugRegistersRequest({3}));
    REQUIRE(request.has_value());
    REQUIRE(request->vcpu == 3);

    mpfshost::DebugRegisterRecord record;
    record.vcpu         = 2;
    record.threadId     = "p01.03";
    record.rip          = 0x1234;
    record.cr3          = 0xabc000;
    record.rax          = 0x55;
    record.r15          = 0x66;
    record.guestPid     = 42;
    record.guestTid     = 100;
    record.processFound = true;
    record.threadFound  = true;

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
    REQUIRE(decoded->records[0].guestPid == 42);
    REQUIRE(decoded->records[0].guestTid == 100);
    REQUIRE(decoded->records[0].processFound);
    REQUIRE(decoded->records[0].threadFound);

    REQUIRE_FALSE(mpfshost::decodeDebugRegistersResponse({}).has_value());
    REQUIRE_FALSE(
        mpfshost::decodeDebugRegistersRequest(std::vector<std::byte> {static_cast<std::byte>(0)}).has_value());
}

TEST_CASE("debug thread requests and responses round trip", "[core][debug][codec]")
{
    REQUIRE(mpfshost::decodeDebugThreadsRequest(mpfshost::encodeDebugThreadsRequest({})).has_value());
    REQUIRE_FALSE(mpfshost::decodeDebugThreadsRequest(std::vector<std::byte> {static_cast<std::byte>(1)}).has_value());

    mpfshost::DebugThreadRecord record;
    record.vcpu         = 2;
    record.threadId     = "p01.03";
    record.rip          = 0x7ff84522b1f1;
    record.rsp          = 0x1234;
    record.cr3          = 0x200123;
    record.processFound = true;
    record.pid          = 42;
    record.threadFound  = true;
    record.tid          = 100;

    const auto decoded = mpfshost::decodeDebugThreadsResponse(mpfshost::encodeDebugThreadsResponse({{record}}));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->threads.size() == 1);
    REQUIRE(decoded->threads[0].vcpu == 2);
    REQUIRE(decoded->threads[0].threadId == "p01.03");
    REQUIRE(decoded->threads[0].rip == record.rip);
    REQUIRE(decoded->threads[0].rsp == record.rsp);
    REQUIRE(decoded->threads[0].cr3 == record.cr3);
    REQUIRE(decoded->threads[0].processFound);
    REQUIRE(decoded->threads[0].pid == 42);
    REQUIRE(decoded->threads[0].threadFound);
    REQUIRE(decoded->threads[0].tid == 100);

    REQUIRE_FALSE(mpfshost::decodeDebugThreadsResponse({}).has_value());
}

TEST_CASE("debug watch requests, hits and ends round trip", "[core][debug][codec]")
{
    const auto request = mpfshost::decodeDebugWatchRequest(mpfshost::encodeDebugWatchRequest(
        {7, 0x12345678, 4, mpfshost::DebugWatchAccess::Write, 3, 1500, "nt", "KiIdleLoop"}));
    REQUIRE(request.has_value());
    REQUIRE(request->pid == 7);
    REQUIRE(request->address == 0x12345678);
    REQUIRE(request->width == 4);
    REQUIRE(request->maxHits == 3);
    REQUIRE(request->timeoutMs == 1500);
    REQUIRE(request->module == "nt");
    REQUIRE(request->symbol == "KiIdleLoop");

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
    hit.guestPid        = 7;
    hit.guestTid        = 100;
    hit.processFound    = true;
    hit.threadFound     = true;

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
    REQUIRE(decodedHit->guestPid == 7);
    REQUIRE(decodedHit->guestTid == 100);
    REQUIRE(decodedHit->processFound);
    REQUIRE(decodedHit->threadFound);

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
    const auto request = mpfshost::decodeDebugBreakpointRequest(mpfshost::encodeDebugBreakpointRequest(
        {9, 0x7ff84522b1f1ull, mpfshost::DebugBreakpointKind::Hardware, 2500, "msvcrt.dll", "memcpy"}));
    REQUIRE(request.has_value());
    REQUIRE(request->pid == 9);
    REQUIRE(request->address == 0x7ff84522b1f1ull);
    REQUIRE(request->kind == mpfshost::DebugBreakpointKind::Hardware);
    REQUIRE(request->timeoutMs == 2500);
    REQUIRE(request->module == "msvcrt.dll");
    REQUIRE(request->symbol == "memcpy");

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
    response.guestPid        = 9;
    response.guestTid        = 100;
    response.processFound    = true;
    response.threadFound     = true;

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
    REQUIRE(decoded->guestPid == 9);
    REQUIRE(decoded->guestTid == 100);
    REQUIRE(decoded->processFound);
    REQUIRE(decoded->threadFound);

    const auto stepRequest = mpfshost::decodeDebugStepRequest(mpfshost::encodeDebugStepRequest({2}));
    REQUIRE(stepRequest.has_value());
    REQUIRE(stepRequest->vcpu == 2);
    REQUIRE(stepRequest->threadId.empty());

    const auto threadStep =
        mpfshost::decodeDebugStepRequest(mpfshost::encodeDebugStepRequest({mpfshost::DebugStepRequest(0, "p01.03")}));
    REQUIRE(threadStep.has_value());
    REQUIRE(threadStep->threadId == "p01.03");

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
    REQUIRE(has(mpfshost::MessageType::DebugThreadsRequest));
    REQUIRE(has(mpfshost::MessageType::DebugWatchRequest));
    REQUIRE(has(mpfshost::MessageType::DebugBreakpointRequest));
    REQUIRE(has(mpfshost::MessageType::DebugStepRequest));
    REQUIRE(has(mpfshost::MessageType::DebugAttachRequest));
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

TEST_CASE("debug threads are delivered through the fake backend", "[core][debug][session]")
{
    DebugFixture                fixture;
    mpfshost::DebugThreadRecord record;
    record.vcpu         = 1;
    record.threadId     = "p01.02";
    record.rip          = 0x1000;
    record.rsp          = 0x2000;
    record.cr3          = 0x3000;
    record.processFound = true;
    record.pid          = 42;
    record.threadFound  = true;
    record.tid          = 100;
    fixture.backend().setDebugThreads({record});
    RunningServer server {fixture.socketPath(), fixture.targets(), fixture.backend()};

    std::string error;
    const auto  threads = mpfshost::debugThreads(fixture.socketPath(), {"vm1"}, error);
    REQUIRE(threads.has_value());
    REQUIRE(threads->size() == 1);
    REQUIRE((*threads)[0].threadId == "p01.02");
    REQUIRE((*threads)[0].pid == 42);
    REQUIRE((*threads)[0].tid == 100);
    REQUIRE(fixture.backend().debugState()->threadsCalls == 1);
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
        {"vm1", 42, 0x2000, 4, mpfshost::DebugWatchAccess::Write, 2, 5000, {}, {}},
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
        fixture.socketPath(), {"vm1", 42, 0x1000, mpfshost::DebugBreakpointKind::Hardware, 5000, {}, {}}, error);
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

TEST_CASE("debug attach is delivered and a closing session detaches it", "[core][debug][session]")
{
    DebugFixture                  fixture;
    mpfshost::DebugAttachResponse response;
    response.status.runState     = mpfshost::DebugRunState::Paused;
    response.status.stubActive   = true;
    response.status.stubOwned    = true;
    response.status.architecture = "x86_64";
    response.stubMode            = "unix";
    response.guestPaused         = true;
    fixture.backend().setDebugAttach(response);
    RunningServer server {fixture.socketPath(), fixture.targets(), fixture.backend()};

    std::string error;
    const auto  attached = mpfshost::debugAttach(fixture.socketPath(), {"vm1"}, {}, error);
    REQUIRE(attached.has_value());
    REQUIRE(attached->stubMode == "unix");
    REQUIRE(attached->guestPaused);
    REQUIRE(attached->status.stubOwned);
    REQUIRE(fixture.backend().debugState()->attachCalls == 1);
    REQUIRE(fixture.backend().debugState()->lastAttachSession != 0);

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (fixture.backend().debugState()->detachSessionCalls.load() == 0
           && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(fixture.backend().debugState()->detachSessionCalls.load() == 1);
    REQUIRE(fixture.backend().debugState()->lastDetachSessionId.load()
            == fixture.backend().debugState()->lastAttachSession);
}

TEST_CASE("a second debug attach is reported as Busy", "[core][debug][session]")
{
    DebugFixture fixture;
    fixture.backend().failDebugAttachWith("another debug session is already attached", false, true);
    RunningServer server {fixture.socketPath(), fixture.targets(), fixture.backend()};

    std::string error;
    const auto  attached = mpfshost::debugAttach(fixture.socketPath(), {"vm1"}, {}, error);
    REQUIRE_FALSE(attached.has_value());
    REQUIRE(error.find("already attached") != std::string::npos);
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
        fixture.socketPath(), {"vm1", 42, 0x2000, 4, mpfshost::DebugWatchAccess::Write, 1, 5000, {}, {}}, {}, error);
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
        fixture.socketPath(), {"vm1", 42, 0x2000, 4, mpfshost::DebugWatchAccess::Write, 1, 5000, {}, {}}, {}, error);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(error.find("another debug operation") != std::string::npos);

    const auto connection = mpfshost::detail::connectAndBind(fixture.socketPath(), "vm1", error);
    REQUIRE(connection.has_value());
    const auto response = mpfshost::detail::exchange(
        connection->fd(),
        mpfshost::MessageType::DebugWatchRequest,
        mpfshost::encodeDebugWatchRequest({42, 0x2000, 4, mpfshost::DebugWatchAccess::Write, 1, 5000, {}, {}}),
        error);
    REQUIRE(response.has_value());
    REQUIRE(response->type == mpfshost::MessageType::ErrorResponse);
    const auto decoded = mpfshost::decodeErrorResponse(response->payload);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->code == mpfshost::ErrorCode::Busy);
}

TEST_CASE("a symbolic debug target reports NotFound instead of Internal", "[core][debug][session]")
{
    DebugFixture fixture;
    fixture.backend().failDebugWatchWith("symbol 'NoSuchSymbol' was not found in module 'nt'", false, true);
    RunningServer server {fixture.socketPath(), fixture.targets(), fixture.backend()};

    std::string error;
    const auto  result =
        mpfshost::debugWatch(fixture.socketPath(),
                             {"vm1", 42, 0, 4, mpfshost::DebugWatchAccess::Write, 1, 5000, "nt", "NoSuchSymbol"},
                             {},
                             error);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(error.find("NoSuchSymbol") != std::string::npos);
    REQUIRE(fixture.backend().debugState()->lastWatchRequest.has_value());
    REQUIRE(fixture.backend().debugState()->lastWatchRequest->module == "nt");
    REQUIRE(fixture.backend().debugState()->lastWatchRequest->symbol == "NoSuchSymbol");

    fixture.backend().failDebugBreakpointWith("module 'ghost' is not loaded", false, false, true);
    const auto breakpoint = mpfshost::debugBreakpoint(
        fixture.socketPath(), {"vm1", 42, 0, mpfshost::DebugBreakpointKind::Software, 5000, "ghost", {}}, error);
    REQUIRE_FALSE(breakpoint.has_value());
    REQUIRE(error.find("ghost") != std::string::npos);
    REQUIRE(fixture.backend().debugState()->lastBreakpointRequest.has_value());
    REQUIRE(fixture.backend().debugState()->lastBreakpointRequest->module == "ghost");

    const auto connection = mpfshost::detail::connectAndBind(fixture.socketPath(), "vm1", error);
    REQUIRE(connection.has_value());
    const auto response = mpfshost::detail::exchange(
        connection->fd(),
        mpfshost::MessageType::DebugBreakpointRequest,
        mpfshost::encodeDebugBreakpointRequest({42, 0, mpfshost::DebugBreakpointKind::Software, 5000, "ghost", {}}),
        error);
    REQUIRE(response.has_value());
    REQUIRE(response->type == mpfshost::MessageType::ErrorResponse);
    const auto decoded = mpfshost::decodeErrorResponse(response->payload);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->code == mpfshost::ErrorCode::NotFound);
}

TEST_CASE("persistent debug resource messages round trip", "[core][debug][codec]")
{
    const auto armBreak = mpfshost::decodeDebugBreakpointArmRequest(
        mpfshost::encodeDebugBreakpointArmRequest({42, 0x1000, mpfshost::DebugBreakpointKind::Hardware}));
    REQUIRE(armBreak.has_value());
    REQUIRE(armBreak->pid == 42);
    REQUIRE(armBreak->address == 0x1000);
    REQUIRE(armBreak->kind == mpfshost::DebugBreakpointKind::Hardware);

    const auto armBreakResponse = mpfshost::decodeDebugBreakpointArmResponse(
        mpfshost::encodeDebugBreakpointArmResponse({7, 42, 0x1000, mpfshost::DebugBreakpointKind::Hardware, 2}));
    REQUIRE(armBreakResponse.has_value());
    REQUIRE(armBreakResponse->id == 7);
    REQUIRE(armBreakResponse->slot == 2);

    const auto armWatch = mpfshost::decodeDebugWatchArmRequest(
        mpfshost::encodeDebugWatchArmRequest({42, 0x2000, 8, mpfshost::DebugWatchAccess::Write}));
    REQUIRE(armWatch.has_value());
    REQUIRE(armWatch->width == 8);

    const auto armWatchResponse = mpfshost::decodeDebugWatchArmResponse(
        mpfshost::encodeDebugWatchArmResponse({9, 42, 0x2000, 8, mpfshost::DebugWatchAccess::Write, 3}));
    REQUIRE(armWatchResponse.has_value());
    REQUIRE(armWatchResponse->slot == 3);

    const auto remove =
        mpfshost::decodeDebugResourceRemoveResponse(mpfshost::encodeDebugResourceRemoveResponse({9, true}));
    REQUIRE(remove.has_value());
    REQUIRE(remove->removed);

    const mpfshost::DebugBreakpointListResponse breakList {{armBreakResponse.value()}};
    const auto                                  decodedBreakList =
        mpfshost::decodeDebugBreakpointListResponse(mpfshost::encodeDebugBreakpointListResponse(breakList));
    REQUIRE(decodedBreakList.has_value());
    REQUIRE(decodedBreakList->breakpoints.size() == 1);
    REQUIRE(decodedBreakList->breakpoints.front().id == 7);

    const mpfshost::DebugWatchListResponse watchList {{armWatchResponse.value()}};
    const auto                             decodedWatchList =
        mpfshost::decodeDebugWatchListResponse(mpfshost::encodeDebugWatchListResponse(watchList));
    REQUIRE(decodedWatchList.has_value());
    REQUIRE(decodedWatchList->watches.size() == 1);
    REQUIRE(decodedWatchList->watches.front().id == 9);

    const mpfshost::DebugWaitHit hit {
        5,
        mpfshost::DebugResourceKind::Watchpoint,
        mpfshost::DebugBreakpointKind::Software,
        0x2000,
        {0, "p01.01", 0x1000, 0x2000, 0, 0, 0, 0x3000, 0, 0},
        "guest-probe.exe",
        0x100,
        true,
        0x1000,
        0x10,
        "GuestProbeMarker",
        true,
        "GuestProbeMarker",
        4
    };
    const auto decodedHit = mpfshost::decodeDebugWaitHit(mpfshost::encodeDebugWaitHit(hit));
    REQUIRE(decodedHit.has_value());
    REQUIRE(decodedHit->id == 5);
    REQUIRE(decodedHit->record.threadId == "p01.01");
    REQUIRE(decodedHit->module == "guest-probe.exe");
    REQUIRE(decodedHit->pdbDisplacement == 4);

    const mpfshost::DebugWaitEnd end {mpfshost::DebugWatchStatus::Completed, 1, 2, {}};
    const auto                   decodedEnd = mpfshost::decodeDebugWaitEnd(mpfshost::encodeDebugWaitEnd(end));
    REQUIRE(decodedEnd.has_value());
    REQUIRE(decodedEnd->status == mpfshost::DebugWatchStatus::Completed);
    REQUIRE(decodedEnd->skippedCount == 2);

    REQUIRE_FALSE(mpfshost::decodeDebugBreakpointArmRequest({}).has_value());
    REQUIRE_FALSE(mpfshost::decodeDebugWaitRequest(std::vector<std::byte> {std::byte {0}}).has_value());
}

TEST_CASE("persistent debug resources are delivered through the fake backend", "[core][debug][session]")
{
    DebugFixture fixture;

    const mpfshost::DebugBreakpointArmResponse breakpoint {11, 42, 0x1000, mpfshost::DebugBreakpointKind::Hardware, 0};
    const mpfshost::DebugWatchArmResponse      watch {12, 42, 0x2000, 4, mpfshost::DebugWatchAccess::Write, 1};
    fixture.backend().setDebugArmBreakpoint(breakpoint);
    fixture.backend().setDebugArmWatch(watch);
    fixture.backend().setDebugListBreakpoints({{breakpoint}});
    fixture.backend().setDebugListWatches({{watch}});
    fixture.backend().setDebugRemoveBreakpoint({11, true});
    fixture.backend().setDebugRemoveWatch({12, true});

    mpfshost::DebugStatus continued;
    continued.runState = mpfshost::DebugRunState::Running;
    fixture.backend().setDebugContinue(continued);

    mpfshost::DebugWaitHit hit;
    hit.id              = 11;
    hit.kind            = mpfshost::DebugResourceKind::Breakpoint;
    hit.address         = 0x1000;
    hit.record.vcpu     = 3;
    hit.record.threadId = "p01.04";
    hit.record.rip      = 0x1000;
    mpfshost::DebugWaitEnd waitEnd;
    waitEnd.status   = mpfshost::DebugWatchStatus::Completed;
    waitEnd.hitCount = 1;
    fixture.backend().setDebugWait({hit}, waitEnd);

    RunningServer server {fixture.socketPath(), fixture.targets(), fixture.backend()};

    std::string error;
    const auto  armedBreak = mpfshost::debugArmBreakpoint(
        fixture.socketPath(), {"vm1", 42, 0x1000, mpfshost::DebugBreakpointKind::Hardware}, error);
    REQUIRE(armedBreak.has_value());
    REQUIRE(armedBreak->id == 11);

    const auto armedWatch =
        mpfshost::debugArmWatch(fixture.socketPath(), {"vm1", 42, 0x2000, 4, mpfshost::DebugWatchAccess::Write}, error);
    REQUIRE(armedWatch.has_value());
    REQUIRE(armedWatch->slot == 1);

    const auto breaks = mpfshost::debugListBreakpoints(fixture.socketPath(), {"vm1"}, error);
    REQUIRE(breaks.has_value());
    REQUIRE(breaks->size() == 1);
    REQUIRE(breaks->front().address == 0x1000);

    const auto watches = mpfshost::debugListWatches(fixture.socketPath(), {"vm1"}, error);
    REQUIRE(watches.has_value());
    REQUIRE(watches->size() == 1);

    const auto status = mpfshost::debugContinueArmed(fixture.socketPath(), {"vm1"}, error);
    REQUIRE(status.has_value());
    REQUIRE(status->runState == mpfshost::DebugRunState::Running);

    std::vector<mpfshost::DebugWaitHit> hits;
    const auto                          waited = mpfshost::debugWait(
        fixture.socketPath(),
        {"vm1", 5000, 1},
        [&hits](const mpfshost::DebugWaitHit& received)
        {
            hits.push_back(received);
        },
        error);
    REQUIRE(waited.has_value());
    REQUIRE(waited->hitCount == 1);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits.front().id == 11);

    const auto removedBreak = mpfshost::debugRemoveBreakpoint(fixture.socketPath(), {"vm1", 11}, error);
    REQUIRE(removedBreak.has_value());
    REQUIRE(removedBreak->removed);
    const auto removedWatch = mpfshost::debugRemoveWatch(fixture.socketPath(), {"vm1", 12}, error);
    REQUIRE(removedWatch.has_value());
    REQUIRE(removedWatch->removed);

    REQUIRE(fixture.backend().debugState()->armBreakpointCalls == 1);
    REQUIRE(fixture.backend().debugState()->armWatchCalls == 1);
    REQUIRE(fixture.backend().debugState()->listBreakpointsCalls == 1);
    REQUIRE(fixture.backend().debugState()->listWatchesCalls == 1);
    REQUIRE(fixture.backend().debugState()->continueCalls == 1);
    REQUIRE(fixture.backend().debugState()->waitCalls == 1);
    REQUIRE(fixture.backend().debugState()->lastArmBreakpointRequest->address == 0x1000);
    REQUIRE(fixture.backend().debugState()->lastArmWatchRequest->width == 4);
}
