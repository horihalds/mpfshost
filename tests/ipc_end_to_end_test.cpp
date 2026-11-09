#include <catch2/catch.hpp>

#include "core/client/client.hpp"
#include "core/server/server.hpp"
#include "core/util/hex.hpp"
#include "support/fake_backend.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
    class EndToEndFixture
    {
    public:
        EndToEndFixture()
        {
            std::filesystem::create_directories(MPFSHOST_TEST_TMP_DIR);
            std::string pattern = (std::filesystem::path {MPFSHOST_TEST_TMP_DIR} / "e2e-XXXXXX").string();
            REQUIRE(::mkdtemp(pattern.data()) != nullptr);
            _root       = pattern;
            _socketPath = _root / "host.sock";
            REQUIRE(_socketPath.string().size() < sizeof(sockaddr_un::sun_path));
        }

        ~EndToEndFixture()
        {
            std::error_code error;
            std::filesystem::remove_all(_root, error);
        }

        EndToEndFixture(const EndToEndFixture&)            = delete;
        EndToEndFixture& operator=(const EndToEndFixture&) = delete;

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
} // namespace

TEST_CASE("client lists the vm targets served by the host", "[ipc][client]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"},
        { "vm-beta", 222,  fixture.root() / "vm-beta.sock"},
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  response = mpfshost::listTargets(fixture.socketPath(), error);

    REQUIRE(response.has_value());
    REQUIRE(error.empty());
    REQUIRE(response->targets.size() == 2);
    REQUIRE(response->targets[0].name == "vm-alpha");
    REQUIRE(response->targets[0].qemuPid == 111);
    REQUIRE(response->targets[0].qmpSocketPath == (fixture.root() / "vm-alpha.sock").string());
    REQUIRE(response->targets[1].name == "vm-beta");
}

TEST_CASE("client discovers the host capabilities without binding", "[ipc][client][capabilities]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    mpfshost::ServerOptions             options {fixture.socketPath()};
    options.allowVfsWrite = true;
    RunningServer server {options, targets, backend};

    std::string error;
    const auto  response = mpfshost::getCapabilities(fixture.socketPath(), error);

    REQUIRE(response.has_value());
    REQUIRE(error.empty());
    REQUIRE_FALSE(response->serverVersion.empty());

    // Every request type named in frame.hpp (everything below the response range) must be advertised.
    for (std::uint32_t value = 0; value <= 0xFFFF; ++value)
    {
        const auto type = static_cast<mpfshost::MessageType>(value);
        if (type >= mpfshost::MessageType::ListTargetsResponse)
        {
            continue;
        }
        if (mpfshost::messageTypeName(type) == "UnknownMessageType")
        {
            continue;
        }
        INFO("request type: " << mpfshost::messageTypeName(type));
        CHECK(std::find(response->requestTypes.begin(), response->requestTypes.end(), value)
              != response->requestTypes.end());
    }
}

TEST_CASE("client binds a known target through the host", "[ipc][client]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  response = mpfshost::bindTarget(fixture.socketPath(), "vm-alpha", error);

    REQUIRE(response.has_value());
    REQUIRE(error.empty());
    REQUIRE(response->sessionId != 0);
    REQUIRE(response->target.name == "vm-alpha");
    REQUIRE(response->target.qemuPid == 111);
    REQUIRE(backend.acquireCount() == 1);
}

TEST_CASE("client reports an unknown target as a failure", "[ipc][client]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    RunningServer                       server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  response = mpfshost::bindTarget(fixture.socketPath(), "ghost", error);

    REQUIRE_FALSE(response.has_value());
    REQUIRE_FALSE(error.empty());
    REQUIRE(backend.acquireCount() == 0);
}

TEST_CASE("client reports an unavailable target as a failure", "[ipc][client]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failWith("device is not ready");
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  response = mpfshost::bindTarget(fixture.socketPath(), "vm-alpha", error);

    REQUIRE_FALSE(response.has_value());
    REQUIRE(error == "device is not ready");
}

TEST_CASE("client fails cleanly when the host is not running", "[ipc][client]")
{
    EndToEndFixture fixture;

    std::string error;
    const auto  response = mpfshost::listTargets(fixture.socketPath(), error);

    REQUIRE_FALSE(response.has_value());
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("client lists the processes of a bound target", "[ipc][client]")
{
    EndToEndFixture                     fixture;
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

    std::string error;
    const auto  response = mpfshost::listProcesses(fixture.socketPath(), {"vm-alpha", 0, ""}, error);

    REQUIRE(response.has_value());
    REQUIRE(error.empty());
    REQUIRE(response->processes.size() == 3);
    REQUIRE(response->processes[0].name == "System");
    REQUIRE(response->processes[1].pid == 100);
    REQUIRE(response->processes[1].parentPid == 4);
    REQUIRE(response->processes[2].wow64);
    REQUIRE(backend.acquireCount() == 1);
}

TEST_CASE("client forwards the process filter to the host", "[ipc][client]")
{
    EndToEndFixture                     fixture;
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

    std::string error;
    const auto  response = mpfshost::listProcesses(fixture.socketPath(), {"vm-alpha", 0, "NOTE"}, error);

    REQUIRE(response.has_value());
    REQUIRE(error.empty());
    REQUIRE(response->processes.size() == 1);
    REQUIRE(response->processes[0].pid == 200);

    const auto filter = backend.lastProcessFilter();
    REQUIRE(filter.has_value());
    REQUIRE(filter->pid == 0);
    REQUIRE(filter->nameFilter == "NOTE");
}

TEST_CASE("client lists every process including terminated ones", "[ipc][client]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setAllProcesses({
        {        {4, 0, "System", false}, 3},
        {{100, 4, "explorer.exe", false}, 0},
        {{200, 100, "notepad.exe", true}, 1},
    });
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  response = mpfshost::listAllProcesses(fixture.socketPath(), {"vm-alpha", 0, ""}, error);

    REQUIRE(response.has_value());
    REQUIRE(error.empty());
    REQUIRE(response->processes.size() == 3);
    REQUIRE(response->processes[0].process.name == "System");
    REQUIRE(response->processes[0].state == 3);
    REQUIRE(response->processes[2].process.wow64);
    REQUIRE(response->processes[2].state == 1);
    REQUIRE(backend.acquireCount() == 1);
}

TEST_CASE("client forwards the all-process filter to the host", "[ipc][client]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setAllProcesses({
        {        {4, 0, "System", false}, 3},
        {{200, 100, "notepad.exe", true}, 1},
    });
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  response = mpfshost::listAllProcesses(fixture.socketPath(), {"vm-alpha", 0, "NOTE"}, error);

    REQUIRE(response.has_value());
    REQUIRE(response->processes.size() == 1);
    REQUIRE(response->processes[0].process.pid == 200);
    REQUIRE(backend.lastAllProcessFilter()->nameFilter == "NOTE");
}

TEST_CASE("client reports an unknown process target as a failure", "[ipc][client]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    mpfshost::testing::FakeBackend      backend;
    RunningServer                       server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  response = mpfshost::listProcesses(fixture.socketPath(), {"ghost", 0, ""}, error);

    REQUIRE_FALSE(response.has_value());
    REQUIRE_FALSE(error.empty());
    REQUIRE(backend.acquireCount() == 0);
}

TEST_CASE("client reads the full process info through the host", "[ipc][client][process]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });

    mpfshost::ProcessInfoResponse expected;
    expected.process        = {200, 100, "notepad.exe", false};
    expected.state          = 1;
    expected.dtb            = 0x1ad000;
    expected.pebVa          = 0x000000a1b2c3d000ull;
    expected.sessionId      = 1;
    expected.integrityLevel = 3;
    expected.sid            = "S-1-5-21-1000";
    expected.imagePath      = "C:\\Windows\\notepad.exe";
    expected.cmdline        = "notepad.exe a.txt";

    mpfshost::testing::FakeBackend backend;
    backend.setProcessInfo(expected);
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  response = mpfshost::processInfo(fixture.socketPath(), {"vm-alpha", 0, "notepad.exe"}, error);

    REQUIRE(response.has_value());
    REQUIRE(error.empty());
    REQUIRE(response->process.pid == 200);
    REQUIRE(response->process.parentPid == 100);
    REQUIRE(response->state == 1);
    REQUIRE(response->dtb == 0x1ad000);
    REQUIRE(response->sessionId == 1);
    REQUIRE(response->integrityLevel == 3);
    REQUIRE(response->sid == "S-1-5-21-1000");
    REQUIRE(response->imagePath == "C:\\Windows\\notepad.exe");
    REQUIRE(response->cmdline == "notepad.exe a.txt");
    REQUIRE(backend.acquireCount() == 1);
}

TEST_CASE("client surfaces a missing process as an error", "[ipc][client][process]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failProcessInfoWith("process 999 is not present", mpfshost::LookupStatus::NotFound);
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  response = mpfshost::processInfo(fixture.socketPath(), {"vm-alpha", 999, {}}, error);

    REQUIRE_FALSE(response.has_value());
    REQUIRE(error == "process 999 is not present");
}

TEST_CASE("client reassembles a read spanning several frames", "[ipc][client][memory]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    const std::uint32_t size = mpfshost::kMaxPayloadLength + 300;
    std::string         error;
    const auto          data = mpfshost::readMemory(fixture.socketPath(), {"vm-alpha", 7, 0x4000, size}, error);

    REQUIRE(data.has_value());
    REQUIRE(error.empty());
    REQUIRE(data->size() == size);
    for (std::size_t i = 0; i < data->size(); ++i)
    {
        REQUIRE((*data)[i] == static_cast<std::byte>((0x4000 + i) & 0xFF));
    }
    REQUIRE(backend.acquireCount() == 1);
}

TEST_CASE("client write splits a large payload into frame-sized requests", "[ipc][client][memory]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    std::vector<std::byte> payload(mpfshost::kMaxPayloadLength + 128);
    for (std::size_t i = 0; i < payload.size(); ++i)
    {
        payload[i] = static_cast<std::byte>(i & 0xFF);
    }

    std::string error;
    const auto  written = mpfshost::writeMemory(fixture.socketPath(), {"vm-alpha", 7, 0x5000, payload}, error);

    REQUIRE(written.has_value());
    REQUIRE(error.empty());
    REQUIRE(*written == payload.size());

    const auto state = backend.memoryState();
    REQUIRE(state->lastWriteAddress == 0x5000 + (payload.size() - 144));
    REQUIRE(state->lastWriteData.size() == 144);
}

TEST_CASE("client surfaces a failed read with the provider's message", "[ipc][client][memory]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failMemoryWith("page is not present");
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  data = mpfshost::readMemory(fixture.socketPath(), {"vm-alpha", 7, 0x4000, 16}, error);

    REQUIRE_FALSE(data.has_value());
    REQUIRE(error == "page is not present");
}

TEST_CASE("client rejects a zero-length read before connecting", "[ipc][client][memory]")
{
    EndToEndFixture fixture;

    std::string error;
    const auto  data = mpfshost::readMemory(fixture.socketPath(), {"vm-alpha", 7, 0x4000, 0}, error);

    REQUIRE_FALSE(data.has_value());
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("client runs a scatter read through the full handle lifecycle", "[ipc][client][scatter]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  results = mpfshost::scatterRead(fixture.socketPath(),
                                                {
                                                    "vm-alpha", 7, {{0x1000, 16}, {0x2000, 8}}
    },
                                                error);

    REQUIRE(results.has_value());
    REQUIRE(error.empty());
    REQUIRE(results->size() == 2);

    REQUIRE((*results)[0].address == 0x1000);
    REQUIRE((*results)[0].requestedSize == 16);
    REQUIRE((*results)[0].bytesRead == 16);
    REQUIRE((*results)[0].ok);
    REQUIRE((*results)[0].data.size() == 16);
    REQUIRE((*results)[0].data[15] == static_cast<std::byte>(0x0F));

    REQUIRE((*results)[1].address == 0x2000);
    REQUIRE((*results)[1].bytesRead == 8);
    REQUIRE((*results)[1].data.size() == 8);

    const auto scatterState = backend.scatterState();
    REQUIRE(scatterState->creates == 1);
    REQUIRE(scatterState->entries.size() == 2);
    REQUIRE_FALSE(scatterState->entries[0].write);
    REQUIRE(scatterState->liveHandles.load() == 0);
}

TEST_CASE("client re-reads and clears a scatter session", "[ipc][client][scatter]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    std::string error;
    auto        session = mpfshost::createScatter(fixture.socketPath(), "vm-alpha", 7, error);
    REQUIRE(session.has_value());
    REQUIRE(error.empty());

    REQUIRE(session
                ->prepare(
                    {
                        {0x1000, 16, false, {}}
    },
                    error)
                .has_value());
    REQUIRE(error.empty());

    const auto first = session->execute(error);
    REQUIRE(first.has_value());
    REQUIRE(first->size() == 1);
    REQUIRE((*first)[0].address == 0x1000);
    REQUIRE((*first)[0].bytesRead == 16);

    const auto second = session->executeRead(error);
    REQUIRE(second.has_value());
    REQUIRE(second->size() == 1);
    REQUIRE((*second)[0].address == 0x1000);
    REQUIRE((*second)[0].bytesRead == 16);
    REQUIRE((*second)[0].data.size() == 16);

    REQUIRE(session->clear(error));
    REQUIRE(error.empty());

    REQUIRE(session->close(error));
    REQUIRE(error.empty());

    const auto scatterState = backend.scatterState();
    REQUIRE(scatterState->creates == 1);
    REQUIRE(scatterState->executeReads == 1);
    REQUIRE(scatterState->clears == 1);
    REQUIRE(scatterState->liveHandles.load() == 0);
}

TEST_CASE("client reassembles a module list spanning several frames", "[ipc][client][module]")
{
    EndToEndFixture                     fixture;
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
        module.name = "module" + std::to_string(index) + std::string(4096, 'x');
        modules.push_back(std::move(module));
    }
    backend.setModules(std::move(modules));
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  listed =
        mpfshost::listModules(fixture.socketPath(), {"vm-alpha", 7, "MODULE1", mpfshost::kModuleFlagDebugInfo}, error);

    REQUIRE(listed.has_value());
    REQUIRE(error.empty());
    REQUIRE(listed->size() == 111);
    REQUIRE((*listed)[0].base == 1);
    REQUIRE((*listed)[110].base == 199);

    const auto last = backend.moduleState()->lastRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->pid == 7);
    REQUIRE(last->flags == mpfshost::kModuleFlagDebugInfo);
    REQUIRE(last->nameFilter == "MODULE1");
}

TEST_CASE("client lists the memory map of a process", "[ipc][client][map]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setRegions({
        {0x10000, 0x1FFFF, 0x04, 0x1000, 1,   mpfshost::kMemoryMapFlagImage, 0, "probe.exe"},
        {0x20000, 0x2FFFF, 0x02, 0x2000, 2, mpfshost::kMemoryMapFlagPrivate, 3,      "heap"},
    });
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  regions = mpfshost::listMemoryMap(fixture.socketPath(), {"vm-alpha", 7, true}, error);

    REQUIRE(regions.has_value());
    REQUIRE(error.empty());
    REQUIRE(regions->size() == 2);
    REQUIRE((*regions)[0].start == 0x10000);
    REQUIRE((*regions)[0].end == 0x1FFFF);
    REQUIRE((*regions)[0].flags == mpfshost::kMemoryMapFlagImage);
    REQUIRE((*regions)[1].heapNumber == 3);

    const auto last = backend.mapState()->lastRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->pid == 7);
    REQUIRE(last->identifyModules);
}

TEST_CASE("client lists the vad-ex pages of a process", "[ipc][client][map]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::VadExDescriptor page;
    page.type      = 2;
    page.pml       = 4;
    page.flags     = mpfshost::kVadExFlagWrite;
    page.va        = 0x100000;
    page.pa        = 0x200000;
    page.pte       = 0x300000;
    page.protoType = 3;
    page.protoPa   = 0x400000;
    page.protoPte  = 0x500000;
    page.vaVadBase = 0x100000;
    backend.setVadExEntries({page});
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  pages = mpfshost::listVadEx(fixture.socketPath(), {"vm-alpha", 7, 8, 2}, error);

    REQUIRE(pages.has_value());
    REQUIRE(error.empty());
    REQUIRE(pages->size() == 1);
    REQUIRE((*pages)[0].va == 0x100000);
    REQUIRE((*pages)[0].pa == 0x200000);
    REQUIRE((*pages)[0].flags == mpfshost::kVadExFlagWrite);

    const auto last = backend.vadExState()->lastRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->pid == 7);
    REQUIRE(last->pageOffset == 8);
    REQUIRE(last->pageCount == 2);
}

TEST_CASE("client reassembles a thread list spanning several frames", "[ipc][client][thread]")
{
    EndToEndFixture                     fixture;
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
        thread.pid = 7;
        mpfshost::ThreadFrame frame;
        frame.function = std::string(4096, 'f');
        thread.frames.push_back(std::move(frame));
        threads.push_back(std::move(thread));
    }
    backend.setThreads(std::move(threads));
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  listed =
        mpfshost::listThreads(fixture.socketPath(), {"vm-alpha", 7, 0, mpfshost::kThreadFlagCallstack}, error);

    REQUIRE(listed.has_value());
    REQUIRE(error.empty());
    REQUIRE(listed->size() == 300);
    REQUIRE((*listed)[0].tid == 1);
    REQUIRE((*listed)[299].tid == 300);
    REQUIRE((*listed)[0].frames.size() == 1);
    REQUIRE((*listed)[0].frames[0].function.size() == 4096);

    const auto last = backend.threadState()->lastRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->pid == 7);
    REQUIRE(last->tid == 0);
    REQUIRE(last->flags == mpfshost::kThreadFlagCallstack);
}

TEST_CASE("client returns a thread list with callstack frames and a failure marker", "[ipc][client][thread]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::ThreadDescriptor running;
    running.tid        = 100;
    running.pid        = 7;
    running.state      = 2;
    running.running    = true;
    running.userTime   = 250;
    running.kernelTime = 125;
    running.affinity   = 3;

    mpfshost::ThreadFrame frame;
    frame.index            = 0;
    frame.returnAddress    = 0x401000;
    frame.stackPointer     = 0x1000;
    frame.frameBase        = 0x2000;
    frame.displacement     = 0x1A;
    frame.registersPresent = true;
    frame.module           = "probe.exe";
    frame.function         = "main";
    running.frames.push_back(frame);

    mpfshost::ThreadDescriptor blocked;
    blocked.tid             = 101;
    blocked.pid             = 7;
    blocked.state           = 5;
    blocked.callstackStatus = 1;

    backend.setThreads({running, blocked});
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  listed =
        mpfshost::listThreads(fixture.socketPath(), {"vm-alpha", 7, 0, mpfshost::kThreadFlagCallstack}, error);

    REQUIRE(listed.has_value());
    REQUIRE(error.empty());
    REQUIRE(listed->size() == 2);
    REQUIRE((*listed)[0].tid == 100);
    REQUIRE((*listed)[0].running);
    REQUIRE((*listed)[0].userTime == 250);
    REQUIRE((*listed)[0].kernelTime == 125);
    REQUIRE((*listed)[0].affinity == 3);
    REQUIRE((*listed)[0].frames.size() == 1);
    REQUIRE((*listed)[0].frames[0].index == 0);
    REQUIRE((*listed)[0].frames[0].returnAddress == 0x401000);
    REQUIRE((*listed)[0].frames[0].stackPointer == 0x1000);
    REQUIRE((*listed)[0].frames[0].frameBase == 0x2000);
    REQUIRE((*listed)[0].frames[0].displacement == 0x1A);
    REQUIRE((*listed)[0].frames[0].registersPresent);
    REQUIRE((*listed)[0].frames[0].module == "probe.exe");
    REQUIRE((*listed)[0].frames[0].function == "main");
    REQUIRE((*listed)[1].tid == 101);
    REQUIRE((*listed)[1].callstackStatus == 1);
    REQUIRE((*listed)[1].frames.empty());
}

TEST_CASE("client returns an empty thread list for an unmatched tid", "[ipc][client][thread]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    mpfshost::ThreadDescriptor     thread;
    thread.tid = 100;
    thread.pid = 7;
    backend.setThreads({thread});
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  listed = mpfshost::listThreads(fixture.socketPath(), {"vm-alpha", 7, 999, 0}, error);

    REQUIRE(listed.has_value());
    REQUIRE(error.empty());
    REQUIRE(listed->empty());

    const auto last = backend.threadState()->lastRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->tid == 999);
    REQUIRE(last->flags == 0);
}

TEST_CASE("client surfaces a failed module listing", "[ipc][client][module]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failModulesWith("module map unavailable");
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  listed = mpfshost::listModules(fixture.socketPath(), {"vm-alpha", 7, "", 0}, error);

    REQUIRE_FALSE(listed.has_value());
    REQUIRE(error == "module map unavailable");
}

TEST_CASE("client surfaces a failed memory map listing", "[ipc][client][map]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failMapsWith("vad map unavailable");
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  regions = mpfshost::listMemoryMap(fixture.socketPath(), {"vm-alpha", 7, false}, error);

    REQUIRE_FALSE(regions.has_value());
    REQUIRE(error == "vad map unavailable");
}

TEST_CASE("client lists the vm partitions of a target", "[ipc][client][physical]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({
        {"physical", 0x800000, 1, 2, 3, 22631, 4242, true, false, false}
    });
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  vms = mpfshost::listVms(fixture.socketPath(), "vm-alpha", error);

    REQUIRE(vms.has_value());
    REQUIRE(error.empty());
    REQUIRE(vms->size() == 1);
    REQUIRE((*vms)[0].name == "physical");
    REQUIRE((*vms)[0].gpaMax == 0x800000);
    REQUIRE((*vms)[0].partitionId == 3);
    REQUIRE((*vms)[0].vmMemPid == 4242);
    REQUIRE((*vms)[0].active);
}

TEST_CASE("client reassembles a guest physical read spanning several frames", "[ipc][client][physical]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({
        {"physical", 0x800000, 1, 2, 0, 22631, 4242, true, false, false}
    });
    RunningServer server {fixture.socketPath(), targets, backend};

    const std::uint32_t size = mpfshost::kMaxPayloadLength + 300;
    std::string         error;
    const auto          data = mpfshost::readPhysicalMemory(fixture.socketPath(), {"vm-alpha", 0, 0x2000, size}, error);

    REQUIRE(data.has_value());
    REQUIRE(error.empty());
    REQUIRE(data->size() == size);
    for (std::size_t i = 0; i < data->size(); ++i)
    {
        REQUIRE((*data)[i] == static_cast<std::byte>((0x2000 + i) & 0xFF));
    }

    const auto state = backend.physicalState();
    REQUIRE(state->lastReadVmIndex == 0);
    REQUIRE(state->lastReadGpa == 0x2000);
    REQUIRE(state->lastReadSize == size);
}

TEST_CASE("client surfaces a failed guest physical read", "[ipc][client][physical]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({
        {"physical", 0x800000, 1, 2, 0, 22631, 4242, true, false, false}
    });
    backend.physicalState()->failure = "gpa not backed by RAM";
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  data = mpfshost::readPhysicalMemory(fixture.socketPath(), {"vm-alpha", 0, 0x2000, 16}, error);

    REQUIRE_FALSE(data.has_value());
    REQUIRE(error == "gpa not backed by RAM");
}

TEST_CASE("client writes guest physical memory", "[ipc][client][physical]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({
        {"physical", 0x800000, 1, 2, 0, 22631, 4242, true, false, false}
    });
    RunningServer server {fixture.socketPath(), targets, backend};

    const std::vector<std::byte> payload {std::byte {0xDE}, std::byte {0xAD}, std::byte {0xBE}, std::byte {0xEF}};

    std::string error;
    const auto  written = mpfshost::writePhysicalMemory(fixture.socketPath(), {"vm-alpha", 0, 0x3000, payload}, error);

    REQUIRE(written.has_value());
    REQUIRE(error.empty());
    REQUIRE(*written == payload.size());

    const auto state = backend.physicalState();
    REQUIRE(state->lastWriteVmIndex == 0);
    REQUIRE(state->lastWriteGpa == 0x3000);
    REQUIRE(state->lastWriteData == payload);
}

TEST_CASE("client translates a gpa inside the partition", "[ipc][client][physical]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({
        {"physical", 0x800000, 1, 2, 0, 22631, 4242, true, false, false}
    });
    backend.physicalState()->translateResponse = {0x9000, 0, true, false};
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  translated = mpfshost::translateGpa(fixture.socketPath(), {"vm-alpha", 0, 0x1000}, error);

    REQUIRE(translated.has_value());
    REQUIRE(error.empty());
    REQUIRE(translated->pa == 0x9000);
    REQUIRE(translated->paPresent);
    REQUIRE_FALSE(translated->vaPresent);
    REQUIRE(backend.physicalState()->lastTranslateGpa == 0x1000);
}

TEST_CASE("client translates a process virtual address to a physical one", "[ipc][client][physical]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.memoryState()->virt2PhysPa = 0xA000;
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  pa = mpfshost::virt2Phys(fixture.socketPath(), {"vm-alpha", 4242, 0x7FF600001000ull}, error);

    REQUIRE(pa.has_value());
    REQUIRE(error.empty());
    REQUIRE(*pa == 0xA000);
    REQUIRE(backend.memoryState()->lastVirt2PhysVa == 0x7FF600001000ull);
}

TEST_CASE("client runs a one-shot physical scatter", "[ipc][client][physical]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({
        {"physical", 0x800000, 1, 2, 0, 22631, 4242, true, false, false}
    });
    backend.physicalState()->failedScatterIndices = {1};
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  results = mpfshost::physicalScatter(
        fixture.socketPath(),
        {
            "vm-alpha", 0, {{0x1000, 16, false, {}}, {0x2000, 8, false, {}}, {0x3000, 8, false, {}}}
    },
        error);

    REQUIRE(results.has_value());
    REQUIRE(error.empty());
    REQUIRE(results->size() == 3);

    REQUIRE((*results)[0].address == 0x1000);
    REQUIRE((*results)[0].requestedSize == 16);
    REQUIRE((*results)[0].bytesRead == 16);
    REQUIRE((*results)[0].ok);
    for (std::size_t i = 0; i < (*results)[0].data.size(); ++i)
    {
        REQUIRE((*results)[0].data[i] == static_cast<std::byte>((0x1000 + i) & 0xFF));
    }

    REQUIRE_FALSE((*results)[1].ok);
    REQUIRE((*results)[1].data.empty());

    REQUIRE((*results)[2].ok);
    REQUIRE((*results)[2].data.size() == 8);

    const auto state = backend.physicalState();
    REQUIRE(state->lastScatterVmIndex == 0);
    REQUIRE(state->lastScatterEntries.size() == 3);
}

TEST_CASE("client surfaces a failed physical scatter", "[ipc][client][physical]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setVms({
        {"physical", 0x800000, 1, 2, 0, 22631, 4242, true, false, false}
    });
    backend.physicalState()->scatterFailure = "scatter engine unavailable";
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  results =
        mpfshost::physicalScatter(fixture.socketPath(), {"vm-alpha", 0, {{0x1000, 8, false, {}}}}, error);

    REQUIRE_FALSE(results.has_value());
    REQUIRE(error == "scatter engine unavailable");
}

TEST_CASE("client resolves a symbol through the host", "[ipc][client][module]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setSymbolResponse({0x7ffb10000000ull, 0x7ffb10012340ull, 0x12340, true});
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  response =
        mpfshost::lookupSymbol(fixture.socketPath(), {"vm-alpha", 4, "kernel32.dll", "CreateFileW"}, error);

    REQUIRE(response.has_value());
    REQUIRE(error.empty());
    REQUIRE(response->moduleBase == 0x7ffb10000000ull);
    REQUIRE(response->address == 0x7ffb10012340ull);
    REQUIRE(response->offset == 0x12340);
    REQUIRE(response->found);
    REQUIRE(backend.acquireCount() == 1);
}

TEST_CASE("client surfaces an unknown module as an error", "[ipc][client][module]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failSymbolWith("module 'ghost.dll' is not loaded in process 4", mpfshost::LookupStatus::NotFound);
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  response = mpfshost::lookupSymbol(fixture.socketPath(), {"vm-alpha", 4, "ghost.dll", {}}, error);

    REQUIRE_FALSE(response.has_value());
    REQUIRE(error == "module 'ghost.dll' is not loaded in process 4");
}

TEST_CASE("client resolves a module by address through the host", "[ipc][client][module]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend    backend;
    mpfshost::ModuleByAddressResponse response;
    response.base      = 0x7ffb10000000ull;
    response.entry     = 0x7ffb10001000ull;
    response.imageSize = 0x2000;
    response.offset    = 0x340;
    response.kind      = 2;
    response.name      = "notepad.exe";
    response.fullName  = "C:\\Windows\\notepad.exe";
    backend.setModuleByAddressResponse(response);
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  decoded = mpfshost::moduleByAddress(fixture.socketPath(), {"vm-alpha", 4321, 0x7ffb10000340ull}, error);

    REQUIRE(decoded.has_value());
    REQUIRE(error.empty());
    REQUIRE(decoded->base == 0x7ffb10000000ull);
    REQUIRE(decoded->offset == 0x340);
    REQUIRE(decoded->name == "notepad.exe");
    REQUIRE(backend.moduleLookupState()->lastAddressRequest->pid == 4321);
}

TEST_CASE("client resolves an address to module, export and pdb symbol", "[ipc][client][module]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::ModuleByAddressResponse module;
    module.base      = 0x7ff8451b0000ull;
    module.entry     = 0x7ff8451b1000ull;
    module.imageSize = 0x200000;
    module.offset    = 0x7b1f1;
    module.kind      = 2;
    module.name      = "msvcrt.dll";
    module.fullName  = "C:\\Windows\\System32\\msvcrt.dll";
    backend.setModuleByAddressResponse(module);
    backend.moduleTableState()->exports = {
        {0x7ff84522b180ull, 1, 0, 0, "memcpy", {}},
        {0x7ff84522b100ull, 2, 0, 0, "memset", {}},
    };
    backend.setPdbSymbolNameResponse({"memcpy_internal", 0x71});
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  resolved = mpfshost::resolveAddress(fixture.socketPath(), {"vm-alpha", 4321, 0x7ff84522b1f1ull}, error);

    REQUIRE(resolved.has_value());
    REQUIRE(error.empty());
    REQUIRE(resolved->moduleFound);
    REQUIRE(resolved->module == "msvcrt.dll");
    REQUIRE(resolved->moduleOffset == 0x7b1f1);
    REQUIRE(resolved->exportFound);
    REQUIRE(resolved->exportName == "memcpy");
    REQUIRE(resolved->exportAddress == 0x7ff84522b180ull);
    REQUIRE(resolved->exportOffset == 0x71);
    REQUIRE(resolved->pdbFound);
    REQUIRE(resolved->pdbSymbol == "memcpy_internal");
    REQUIRE(resolved->pdbDisplacement == 0x71);
    REQUIRE(backend.moduleLookupState()->lastAddressRequest->pid == 4321);
    REQUIRE(backend.pdbState()->lastNameRequest->address == 0x7b1f1);
}

TEST_CASE("client reports an address outside every module as unresolved", "[ipc][client][module]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failModuleByAddressWith("address is not inside a module", mpfshost::LookupStatus::NotFound);
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  resolved = mpfshost::resolveAddress(fixture.socketPath(), {"vm-alpha", 4321, 0x1234}, error);

    REQUIRE(resolved.has_value());
    REQUIRE(error.empty());
    REQUIRE_FALSE(resolved->moduleFound);
}

TEST_CASE("client finds a process by dtb through the host", "[ipc][client][process]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setProcessByDtbResponse({
        {1234, 4, "notepad.exe", false},
        0x1aa000, 0x1ab000, true
    });
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  decoded = mpfshost::processByDtb(fixture.socketPath(), {"vm-alpha", 0x1aafff}, error);

    REQUIRE(decoded.has_value());
    REQUIRE(error.empty());
    REQUIRE(decoded->process.pid == 1234);
    REQUIRE(decoded->dtb == 0x1aa000);
    REQUIRE(decoded->dtbUser == 0x1ab000);
    REQUIRE(decoded->matchedUser);
    REQUIRE(backend.processInfoState()->lastDtbRequest->dtb == 0x1aafff);
}

TEST_CASE("client performs pdb lookups through the host", "[ipc][client][module]")
{
    EndToEndFixture                     fixture;
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

    std::string error;

    const auto load = mpfshost::pdbLoad(fixture.socketPath(), {"vm-alpha", 4, 0xfffff80000000000ull}, error);
    REQUIRE(load.has_value());
    REQUIRE(error.empty());
    REQUIRE(load->module == "nt");
    REQUIRE(backend.pdbState()->lastLoadRequest->pid == 4);

    const auto name = mpfshost::pdbSymbolName(fixture.socketPath(), {"vm-alpha", "nt", 0xfffff80000123456ull}, error);
    REQUIRE(name.has_value());
    REQUIRE(error.empty());
    REQUIRE(name->symbol == "PsGetCurrentProcess");
    REQUIRE(name->displacement == 4);

    const auto address =
        mpfshost::pdbSymbolAddress(fixture.socketPath(), {"vm-alpha", "nt", "PsGetCurrentProcess"}, error);
    REQUIRE(address.has_value());
    REQUIRE(error.empty());
    REQUIRE(address->address == 0xfffff80000123456ull);

    const auto typeSize = mpfshost::pdbTypeSize(fixture.socketPath(), {"vm-alpha", "nt", "_EPROCESS"}, error);
    REQUIRE(typeSize.has_value());
    REQUIRE(error.empty());
    REQUIRE(typeSize->size == 0x4d0);
    REQUIRE(backend.pdbState()->lastTypeSizeRequest->type == "_EPROCESS");

    const auto childOffset =
        mpfshost::pdbTypeChildOffset(fixture.socketPath(), {"vm-alpha", "nt", "_EPROCESS", "UniqueProcessId"}, error);
    REQUIRE(childOffset.has_value());
    REQUIRE(error.empty());
    REQUIRE(childOffset->offset == 0x440);
    REQUIRE(backend.pdbState()->lastChildOffsetRequest->child == "UniqueProcessId");
    REQUIRE(backend.acquireCount() == 5);
}

TEST_CASE("client surfaces a missing pdb as an error", "[ipc][client][module]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failPdbSymbolAddressWith("no pdb symbol named 'ghost' in module 'nt'", mpfshost::LookupStatus::NotFound);
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  address = mpfshost::pdbSymbolAddress(fixture.socketPath(), {"vm-alpha", "nt", "ghost"}, error);

    REQUIRE_FALSE(address.has_value());
    REQUIRE(error == "no pdb symbol named 'ghost' in module 'nt'");
}

TEST_CASE("client reads a module layout through the host", "[ipc][client][module]")
{
    EndToEndFixture                     fixture;
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

    std::string error;
    const auto  response = mpfshost::moduleLayout(fixture.socketPath(), {"vm-alpha", 4, "notepad.exe"}, error);

    REQUIRE(response.has_value());
    REQUIRE(error.empty());
    REQUIRE(response->moduleBase == 0x00007ff700000000ull);
    REQUIRE(response->sections.size() == 1);
    REQUIRE(response->sections[0].name == ".text");
    REQUIRE(response->directories.size() == mpfshost::kModuleDataDirectoryCount);
    REQUIRE(backend.acquireCount() == 1);
}

TEST_CASE("client surfaces an unknown module layout as an error", "[ipc][client][module]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.failLayoutWith("module 'ghost.dll' is not loaded in process 4", mpfshost::LookupStatus::NotFound);
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  response = mpfshost::moduleLayout(fixture.socketPath(), {"vm-alpha", 4, "ghost.dll"}, error);

    REQUIRE_FALSE(response.has_value());
    REQUIRE(error == "module 'ghost.dll' is not loaded in process 4");
}

TEST_CASE("client reports a streamed response the host abandoned", "[ipc][client]")
{
    EndToEndFixture fixture;

    const int listenFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    REQUIRE(listenFd >= 0);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    const auto path    = fixture.socketPath().string();
    REQUIRE(path.size() < sizeof(address.sun_path));
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    REQUIRE(::bind(listenFd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(listenFd, 1) == 0);

    // Answers the implicit bind, sends one data frame and then drops the connection without an end frame.
    std::jthread responder {
        [listenFd]
        {
            const int fd = ::accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd < 0)
            {
                return;
            }

            mpfshost::FrameHeader  header;
            std::vector<std::byte> payload;
            std::string            error;
            if (mpfshost::readFrame(fd, header, payload, error) == mpfshost::FrameReadStatus::Complete)
            {
                const auto bind = mpfshost::encodeBindResponse({
                    1, {"vm-alpha", 111, "/tmp/vm-alpha.sock"}
                });
                (void)mpfshost::writeFrame(fd, mpfshost::MessageType::BindResponse, header.requestId, bind, error);
            }
            if (mpfshost::readFrame(fd, header, payload, error) == mpfshost::FrameReadStatus::Complete)
            {
                const std::vector<std::byte> data(16, std::byte {7});
                (void)mpfshost::writeFrame(fd, mpfshost::MessageType::MemoryReadData, header.requestId, data, error);
            }
            ::close(fd);
        }};

    std::string error;
    const auto  result =
        mpfshost::readMemory(fixture.socketPath(), mpfshost::MemoryQuery {"vm-alpha", 4, 0x1000, 16}, error);

    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(error.empty());
    REQUIRE(error.find("abandoned") != std::string::npos);

    ::close(listenFd);
}

TEST_CASE("client describes a virtual region through the host", "[ipc][client][vquery]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::VirtualRegion region;
    region.baseAddress = 0x10000;
    region.regionSize  = 0x3000;
    region.state       = 0x1000;
    region.protect     = 0x04;
    region.type        = 0x20000;
    region.partitionId = 1;
    backend.setVirtualRegion(region);
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  result = mpfshost::queryVirtual(fixture.socketPath(), {"vm-alpha", 7, 0x10fff}, error);

    REQUIRE(result.has_value());
    REQUIRE(error.empty());
    REQUIRE(result->baseAddress == 0x10000);
    REQUIRE(result->regionSize == 0x3000);
    REQUIRE(result->protect == 0x04);
    REQUIRE(result->partitionId == 1);

    const auto last = backend.virtualQueryState()->lastRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->pid == 7);
    REQUIRE(last->address == 0x10fff);
}

TEST_CASE("client reassembles a page read spanning several frames", "[ipc][client][page]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    // 1300 pages force a second data frame: one frame holds at most 512 KiB of page items.
    std::vector<mpfshost::PageReadItem> pages;
    for (std::uint64_t index = 0; index < 1300; ++index)
    {
        mpfshost::PageReadItem page;
        page.address = 0x10000 + index * 0x1000;
        page.ok      = true;
        page.data    = std::vector<std::byte>(mpfshost::kPageSize, std::byte {0x41});
        pages.push_back(std::move(page));
    }
    backend.setPageItems(pages);
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  result = mpfshost::readPages(fixture.socketPath(), {"vm-alpha", 7, {{0x10000, 1300 * 0x1000}}}, error);

    REQUIRE(result.has_value());
    REQUIRE(error.empty());
    REQUIRE(result->size() == 1300);
    REQUIRE((*result)[0].address == 0x10000);
    REQUIRE((*result)[1299].address == 0x10000 + 1299 * 0x1000);
    REQUIRE((*result)[1299].data.size() == mpfshost::kPageSize);

    const auto last = backend.pageState()->lastAddresses;
    REQUIRE(last.has_value());
    REQUIRE(last->size() == 1300);
    REQUIRE(backend.pageState()->lastPid == 7);
}

TEST_CASE("client prefetches a range through the host", "[ipc][client][page]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  count = mpfshost::prefetchPages(fixture.socketPath(), {"vm-alpha", 7, {{0x2000, 0x3000}}}, error);

    REQUIRE(count.has_value());
    REQUIRE(error.empty());
    REQUIRE(*count == 3);

    const auto last = backend.pageState()->lastAddresses;
    REQUIRE(last.has_value());
    REQUIRE(*last == std::vector<std::uint64_t> {0x2000, 0x3000, 0x4000});
}

TEST_CASE("client sets a config option through the host", "[ipc][client][config]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  applied =
        mpfshost::setConfig(fixture.socketPath(), {"vm-alpha", mpfshost::ConfigOption::ReadCacheTicks, 0, 0x20}, error);

    REQUIRE(applied.has_value());
    REQUIRE(error.empty());
    REQUIRE(*applied == 0x20u);

    const auto last = backend.configState()->lastRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->option == mpfshost::ConfigOption::ReadCacheTicks);
    REQUIRE(last->value == 0x20u);
}

TEST_CASE("client lists services through the host", "[ipc][client][services]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::ServiceDescriptor service;
    service.name                      = "DemoSvc";
    service.account                   = "LocalSystem";
    backend.serviceState()->services  = {service};
    backend.serviceState()->truncated = true;
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  result = mpfshost::listServices(fixture.socketPath(), {"vm-alpha", "demo"}, error);

    REQUIRE(result.has_value());
    REQUIRE(error.empty());
    REQUIRE(result->services.size() == 1);
    REQUIRE(result->services[0].name == "DemoSvc");
    REQUIRE(result->truncated);
    REQUIRE(backend.serviceState()->lastRequest.has_value());
    REQUIRE(backend.serviceState()->lastRequest->filter == "demo");
}

TEST_CASE("client lists users through the host", "[ipc][client][users]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.userState()->users = {
        {"Administrator", 0xFFFFA0001000ull, "S-1-5-21-1"}
    };
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  result = mpfshost::listUsers(fixture.socketPath(), {"vm-alpha", "S-1-5"}, error);

    REQUIRE(result.has_value());
    REQUIRE(error.empty());
    REQUIRE_FALSE(result->truncated);
    REQUIRE(result->users.size() == 1);
    REQUIRE(result->users[0].name == "Administrator");
    REQUIRE(result->users[0].regHive == 0xFFFFA0001000ull);
    REQUIRE(backend.userState()->lastRequest.has_value());
    REQUIRE(backend.userState()->lastRequest->filter == "S-1-5");
}

TEST_CASE("client lists connections through the host with a pid scope", "[ipc][client][net]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::NetworkEntryDescriptor entry;
    entry.pid                       = 4321;
    entry.process                   = "demo.exe";
    entry.local.valid               = true;
    entry.local.address             = "10.0.0.5:445";
    backend.networkState()->entries = {entry, mpfshost::NetworkEntryDescriptor {}};
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  result = mpfshost::listNetwork(fixture.socketPath(), {"vm-alpha", 4321, ""}, error);

    REQUIRE(result.has_value());
    REQUIRE(error.empty());
    REQUIRE(result->entries.size() == 1);
    REQUIRE(result->entries[0].pid == 4321);
    REQUIRE(result->entries[0].local.address == "10.0.0.5:445");
    REQUIRE(backend.networkState()->lastRequest.has_value());
    REQUIRE(backend.networkState()->lastRequest->pid == 4321);
}

TEST_CASE("client lists the heaps of a process", "[ipc][client][heap]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::HeapDescriptor heap;
    heap.address               = 0x000001c000010000ull;
    heap.index                 = 2;
    heap.number                = 7;
    heap.type                  = 1;
    heap.is32Bit               = 0;
    backend.heapState()->heaps = {heap};
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  result = mpfshost::listHeaps(fixture.socketPath(), {"vm-alpha", 4321}, error);

    REQUIRE(result.has_value());
    REQUIRE(error.empty());
    REQUIRE(result->heaps.size() == 1);
    REQUIRE(result->heaps[0].index == 2);
    REQUIRE(result->heaps[0].number == 7);
    REQUIRE(result->heaps[0].type == 1);
    REQUIRE(result->heaps[0].address == 0x000001c000010000ull);
    REQUIRE(backend.heapState()->lastRequest.has_value());
    REQUIRE(backend.heapState()->lastRequest->pid == 4321);
}

TEST_CASE("client lists the allocations of one heap", "[ipc][client][heap]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::HeapAllocDescriptor allocation;
    allocation.address                    = 0x000001c000020000ull;
    allocation.size                       = 0x1000;
    allocation.type                       = 5;
    backend.heapAllocState()->allocations = {allocation};
    backend.heapAllocState()->truncated   = true;
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  result =
        mpfshost::listHeapAllocations(fixture.socketPath(), {"vm-alpha", 4321, 0x000001c000010000ull}, error);

    REQUIRE(result.has_value());
    REQUIRE(error.empty());
    REQUIRE(result->allocations.size() == 1);
    REQUIRE(result->allocations[0].size == 0x1000);
    REQUIRE(result->allocations[0].type == 5);
    REQUIRE(result->truncated);
    REQUIRE(backend.heapAllocState()->lastRequest.has_value());
    REQUIRE(backend.heapAllocState()->lastRequest->pid == 4321);
    REQUIRE(backend.heapAllocState()->lastRequest->heap == 0x000001c000010000ull);
}

TEST_CASE("client surfaces a failed heap listing", "[ipc][client][heap]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.heapState()->failure = "process 4321 is not present";
    backend.heapState()->status  = mpfshost::LookupStatus::NotFound;
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  result = mpfshost::listHeaps(fixture.socketPath(), {"vm-alpha", 4321}, error);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(error == "process 4321 is not present");
}

TEST_CASE("client reports an empty heap listing as a success", "[ipc][client][heap]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  result = mpfshost::listHeaps(fixture.socketPath(), {"vm-alpha", 4}, error);

    REQUIRE(result.has_value());
    REQUIRE(error.empty());
    REQUIRE(result->heaps.empty());
    REQUIRE_FALSE(result->truncated);
}

TEST_CASE("client surfaces a failed service listing", "[ipc][client][services]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.serviceState()->failure = "services are not parsed";
    backend.serviceState()->status  = mpfshost::LookupStatus::Failed;
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  result = mpfshost::listServices(fixture.socketPath(), {"vm-alpha", ""}, error);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(error == "services are not parsed");
}

TEST_CASE("client streams the hits of a memory search", "[ipc][client][search]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setSearchHits({0x1000, 0x2000, 0x3000}, true);
    RunningServer server {fixture.socketPath(), targets, backend};

    const auto term = mpfshost::parseHexPattern("de ad");
    REQUIRE(term.has_value());

    std::string error;
    const auto  result =
        mpfshost::searchMemory(fixture.socketPath(), {"vm-alpha", 7, *term, 2, 0x1000, 0x4000, 16}, error);

    REQUIRE(result.has_value());
    REQUIRE(error.empty());
    REQUIRE(result->addresses == std::vector<std::uint64_t> {0x1000, 0x2000, 0x3000});
    REQUIRE(result->truncated);

    const auto last = backend.searchState()->lastRequest;
    REQUIRE(last.has_value());
    REQUIRE(last->pid == 7);
    REQUIRE(last->alignBytes == 2);
    REQUIRE(last->minAddress == 0x1000);
    REQUIRE(last->maxAddress == 0x4000);
    REQUIRE(last->maxResults == 16);
    REQUIRE(last->term.pattern.size() == 2);
}

TEST_CASE("client lists kernel drivers through the host", "[ipc][client][kerneldrivers]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::KernelDriverDescriptor driver;
    driver.name                            = "Ntfs";
    driver.path                            = "\\SystemRoot\\System32\\drivers\\ntfs.sys";
    driver.serviceKeyName                  = "Ntfs";
    driver.majorFunctions                  = {0x1000, 0x2000};
    backend.kernelDriverState()->drivers   = {driver};
    backend.kernelDriverState()->truncated = true;
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  result = mpfshost::listKernelDrivers(fixture.socketPath(), {"vm-alpha", "ntfs"}, error);

    REQUIRE(result.has_value());
    REQUIRE(error.empty());
    REQUIRE(result->drivers.size() == 1);
    REQUIRE(result->drivers[0].name == "Ntfs");
    REQUIRE(result->drivers[0].majorFunctions.size() == 2);
    REQUIRE(result->truncated);
    REQUIRE(backend.kernelDriverState()->lastRequest.has_value());
    REQUIRE(backend.kernelDriverState()->lastRequest->filter == "ntfs");
}

TEST_CASE("client lists kernel devices through the host", "[ipc][client][kerneldevices]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::KernelDeviceDescriptor device;
    device.va                            = 0xffffc10a00000000ull;
    device.deviceTypeName                = "FILE_DEVICE_DISK";
    backend.kernelDeviceState()->devices = {device};
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  result = mpfshost::listKernelDevices(fixture.socketPath(), {"vm-alpha", "disk"}, error);

    REQUIRE(result.has_value());
    REQUIRE(error.empty());
    REQUIRE(result->devices.size() == 1);
    REQUIRE(result->devices[0].deviceTypeName == "FILE_DEVICE_DISK");
    REQUIRE_FALSE(result->truncated);
    REQUIRE(backend.kernelDeviceState()->lastRequest.has_value());
    REQUIRE(backend.kernelDeviceState()->lastRequest->filter == "disk");
}

TEST_CASE("client lists kernel objects with their capped children", "[ipc][client][kernelobjects]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::KernelObjectDescriptor object;
    object.name       = "\\Device\\HarddiskVolume1";
    object.type       = "Device";
    object.childCount = 1000;
    object.children.assign(mpfshost::kMaxKernelObjectChildren, 0xffffc10a00100000ull);
    object.childrenTruncated             = true;
    backend.kernelObjectState()->objects = {object};
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  result = mpfshost::listKernelObjects(fixture.socketPath(), {"vm-alpha", "device"}, error);

    REQUIRE(result.has_value());
    REQUIRE(error.empty());
    REQUIRE(result->objects.size() == 1);
    REQUIRE(result->objects[0].childCount == 1000);
    REQUIRE(result->objects[0].children.size() == mpfshost::kMaxKernelObjectChildren);
    REQUIRE(result->objects[0].childrenTruncated);
    REQUIRE(backend.kernelObjectState()->lastRequest.has_value());
    REQUIRE(backend.kernelObjectState()->lastRequest->filter == "device");
}

TEST_CASE("client surfaces a failed kernel driver listing", "[ipc][client][kerneldrivers]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.kernelDriverState()->failure = "VMMDLL_Map_GetKDriverU failed";
    backend.kernelDriverState()->status  = mpfshost::LookupStatus::Failed;
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  result = mpfshost::listKernelDrivers(fixture.socketPath(), {"vm-alpha", ""}, error);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(error == "VMMDLL_Map_GetKDriverU failed");
}

TEST_CASE("client reports a missing kernel map provider", "[ipc][client][kerneldevices]")
{
    EndToEndFixture                     fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    backend.setHandlesBare(true);
    RunningServer server {fixture.socketPath(), targets, backend};

    std::string error;
    const auto  result = mpfshost::listKernelDevices(fixture.socketPath(), {"vm-alpha", ""}, error);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(error == "the bound target cannot list kernel devices");
}
