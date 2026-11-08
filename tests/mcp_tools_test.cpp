#include <catch2/catch.hpp>

#include "core/mcp/mcp_server.hpp"
#include "core/server/server.hpp"
#include "support/fake_backend.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    class McpFixture
    {
    public:
        McpFixture()
        {
            std::filesystem::create_directories(MPFSHOST_TEST_TMP_DIR);
            std::string pattern = (std::filesystem::path {MPFSHOST_TEST_TMP_DIR} / "mcp-XXXXXX").string();
            REQUIRE(::mkdtemp(pattern.data()) != nullptr);
            _root       = pattern;
            _socketPath = _root / "host.sock";
            REQUIRE(_socketPath.string().size() < 100);
        }

        ~McpFixture()
        {
            std::error_code error;
            std::filesystem::remove_all(_root, error);
        }

        McpFixture(const McpFixture&)            = delete;
        McpFixture& operator=(const McpFixture&) = delete;

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

    std::vector<std::string> runMcp(const std::vector<std::string>& messages, const std::filesystem::path& socket)
    {
        std::string input;
        for (const auto& message : messages)
        {
            input += message;
            input += '\n';
        }

        std::istringstream       in {input};
        std::ostringstream       out;
        mpfshost::mcp::McpServer server {in, out, {socket}};
        REQUIRE(server.run() == 0);

        std::vector<std::string> lines;
        std::istringstream       output {out.str()};
        std::string              line;
        while (std::getline(output, line))
        {
            lines.push_back(line);
        }
        return lines;
    }

    constexpr const char* kInitialize =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"test","version":"1"}}})";
    constexpr const char* kInitialized = R"({"jsonrpc":"2.0","method":"notifications/initialized"})";

    std::string toolCall(int id, std::string_view name, std::string_view arguments = "{}")
    {
        return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":"tools/call","params":{"name":")"
             + std::string {name} + R"(","arguments":)" + std::string {arguments} + "}}";
    }

    std::vector<std::byte> bytesOf(std::string_view text)
    {
        std::vector<std::byte> bytes;
        for (const char character : text)
        {
            bytes.push_back(static_cast<std::byte>(character));
        }
        return bytes;
    }
} // namespace

TEST_CASE("MCP read tools work against a real host with the fake backend", "[mcp][tools]")
{
    McpFixture                          fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    backend.setProcesses({
        {100, 4, "alpha.exe", false},
        {200, 4,  "beta.exe",  true}
    });
    backend.setModules({
        {0x10000, 0x11000, 0x2000, 0, 1, 2, 3, 4, false, "alpha.exe", "C:\\alpha.exe", "", "", ""}
    });
    backend.setMemoryBytes(bytesOf("Hello, world!"));
    backend.setVfsEntries({
        {"file.txt", 13, 0, 0, 0, false}
    });
    backend.setVfsFile("\\file.txt", bytesOf("Hello, world!"));
    backend.setRegions({
        {0x1000, 0x1fff, 4, 0x2000, 2, 0x4, 0, "image"}
    });
    backend.setVms({
        {"guest", 0x1'0000'0000, 1, 1, 1, 19045, 0, true, false, false}
    });
    backend.setPfnEntries({
        {0x100, 1, 2, 100, 0x2000, 0x3000, 0x4000, 0x500, 7, 3, 2, 1, 0}
    });
    backend.setSearchHits({0x2000, 0x3000}, true);
    backend.kernelDriverState()->drivers = {
        {0x1000, 0x1000, 0x2000, 0x3000, {0x4000}, "ntfs.sys", "C:\\ntfs.sys", "ntfs"},
    };

    RunningServer server {fixture.socketPath(), targets, backend};

    const auto lines = runMcp(
        {
            kInitialize,
            kInitialized,
            toolCall(2, "list_targets"),
            toolCall(3, "list_processes", R"({"target":"vm-alpha","limit":1})"),
            toolCall(4, "read_memory", R"({"target":"vm-alpha","pid":100,"address":"0x1000","size":13})"),
            toolCall(5, "list_modules", R"({"target":"vm-alpha","pid":100})"),
            toolCall(6, "list_vfs", R"({"target":"vm-alpha","path":"\\"})"),
            toolCall(7, "read_vfs", R"({"target":"vm-alpha","path":"\\file.txt","size":8})"),
            toolCall(8, "memory_map", R"({"target":"vm-alpha","pid":100})"),
            toolCall(9, "list_vms", R"({"target":"vm-alpha"})"),
            toolCall(10, "list_kernel_drivers", R"({"target":"vm-alpha"})"),
            toolCall(11, "list_pfn", R"({"target":"vm-alpha","pfn_start":256,"pfn_count":1})"),
            toolCall(12, "search_memory", R"({"target":"vm-alpha","pid":100,"pattern":"Hi"})"),
        },
        fixture.socketPath());

    REQUIRE(lines.size() == 12);
    for (const auto& line : lines)
    {
        INFO(line);
        CHECK(line.find("\"jsonrpc\":\"2.0\"") != std::string::npos);
    }

    CHECK(lines[1].find("vm-alpha") != std::string::npos);
    CHECK(lines[1].find("\"qemu_pid\":111") != std::string::npos);

    CHECK(lines[2].find("\"total\":2") != std::string::npos);
    CHECK(lines[2].find("\"truncated\":true") != std::string::npos);
    CHECK(lines[2].find("alpha.exe") != std::string::npos);
    CHECK(lines[2].find("beta.exe") == std::string::npos);

    CHECK(lines[3].find("\"hex\":\"48656c6c6f2c20776f726c6421\"") != std::string::npos);
    CHECK(lines[3].find("\"ascii\":\"Hello, world!\"") != std::string::npos);

    CHECK(lines[4].find("alpha.exe") != std::string::npos);
    CHECK(lines[4].find("\"base\":\"0x0000000000010000\"") != std::string::npos);

    CHECK(lines[5].find("file.txt") != std::string::npos);
    CHECK(lines[6].find("\"hex\":\"48656c6c6f2c2077\"") != std::string::npos);

    CHECK(lines[7].find("image") != std::string::npos);
    CHECK(lines[8].find("\"name\":\"guest\"") != std::string::npos);
    CHECK(lines[9].find("ntfs.sys") != std::string::npos);
    CHECK(lines[10].find("\"pfn\":256") != std::string::npos);
    CHECK(lines[11].find("\"truncated\":true") != std::string::npos);
    CHECK(lines[11].find("\"items\":[\"0x0000000000002000\",\"0x0000000000003000\"]") != std::string::npos);
}

TEST_CASE("MCP exposes module-by-address and process-by-dtb lookups", "[mcp][tools]")
{
    McpFixture                          fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;

    mpfshost::ModuleByAddressResponse module;
    module.base      = 0x7ffb10000000ull;
    module.entry     = 0x7ffb10001000ull;
    module.imageSize = 0x2000;
    module.offset    = 0x340;
    module.kind      = 2;
    module.name      = "notepad.exe";
    module.fullName  = "C:\\Windows\\notepad.exe";
    backend.setModuleByAddressResponse(module);
    backend.setProcessByDtbResponse({
        {1234, 4, "notepad.exe", false},
        0x1aa000, 0x1ab000, true
    });

    RunningServer server {fixture.socketPath(), targets, backend};

    const auto lines =
        runMcp({kInitialize,
                kInitialized,
                toolCall(2, "module_at_address", R"({"target":"vm-alpha","pid":4321,"address":"0x7ffb10000340"})"),
                toolCall(3, "process_for_dtb", R"({"target":"vm-alpha","dtb":"0x1aafff"})")},
               fixture.socketPath());

    REQUIRE(lines.size() == 3);
    CHECK(lines[1].find("\"name\":\"notepad.exe\"") != std::string::npos);
    CHECK(lines[1].find("\"offset\":\"0x0000000000000340\"") != std::string::npos);
    CHECK(lines[2].find("\"pid\":1234") != std::string::npos);
    CHECK(lines[2].find("\"matched_user\":true") != std::string::npos);
}

TEST_CASE("MCP maps host and transport failures to isError results", "[mcp][tools][errors]")
{
    SECTION("LookupStatus::NotFound")
    {
        McpFixture                          fixture;
        mpfshost::testing::FakeTargetSource targets;
        targets.setTargets({
            {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
        });
        mpfshost::testing::FakeBackend backend;
        backend.failProcessInfoWith("process vanished", mpfshost::LookupStatus::NotFound);
        RunningServer server {fixture.socketPath(), targets, backend};

        const auto lines =
            runMcp({kInitialize, kInitialized, toolCall(2, "process_info", R"({"target":"vm-alpha","pid":123})")},
                   fixture.socketPath());

        REQUIRE(lines.size() == 2);
        CHECK(lines[1].find("\"isError\":true") != std::string::npos);
        CHECK(lines[1].find("process vanished") != std::string::npos);
    }

    SECTION("unreachable socket")
    {
        const auto lines = runMcp({kInitialize, kInitialized, toolCall(2, "list_targets")},
                                  std::filesystem::path {MPFSHOST_TEST_TMP_DIR} / "no-such-mcp-host.sock");
        REQUIRE(lines.size() == 2);
        CHECK(lines[1].find("\"isError\":true") != std::string::npos);
    }

    SECTION("capability unavailable")
    {
        McpFixture                          fixture;
        mpfshost::testing::FakeTargetSource targets;
        targets.setTargets({
            {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
        });
        mpfshost::testing::FakeBackend backend;
        backend.setHandlesBare(true);
        RunningServer server {fixture.socketPath(), targets, backend};

        const auto lines =
            runMcp({kInitialize, kInitialized, toolCall(2, "list_processes", R"({"target":"vm-alpha"})")},
                   fixture.socketPath());

        REQUIRE(lines.size() == 2);
        CHECK(lines[1].find("\"isError\":true") != std::string::npos);
        CHECK(lines[1].find("cannot list processes") != std::string::npos);
    }
}

TEST_CASE("MCP write tools are always available", "[mcp][tools][write]")
{
    McpFixture                          fixture;
    mpfshost::testing::FakeTargetSource targets;
    targets.setTargets({
        {"vm-alpha", 111, fixture.root() / "vm-alpha.sock"}
    });
    mpfshost::testing::FakeBackend backend;
    RunningServer                  server {fixture.socketPath(), targets, backend};

    const auto lines =
        runMcp({kInitialize,
                kInitialized,
                toolCall(2, "write_memory", R"({"target":"vm-alpha","pid":100,"address":"0x1000","data":"deadbeef"})")},
               fixture.socketPath());
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].find("\"isError\":false") != std::string::npos);
    CHECK(lines[1].find("\"bytes_written\":\"0x0000000000000004\"") != std::string::npos);
    REQUIRE(backend.memoryState()->lastWriteData.size() == 4);
    CHECK(backend.memoryState()->lastWriteAddress == 0x1000);
    CHECK(backend.memoryState()->lastWritePid == 100);
}
