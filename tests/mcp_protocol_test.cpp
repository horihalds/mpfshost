#include <catch2/catch.hpp>

#include "core/mcp/mcp_server.hpp"
#include "core/mcp/mcp_tools.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    struct McpRun
    {
        int                      status {};
        std::vector<std::string> lines;
    };

    McpRun runMcp(const std::vector<std::string>& messages,
                  const std::filesystem::path&    socket = "/nonexistent/mpfshost.sock")
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
        McpRun                   run;
        run.status = server.run();

        std::istringstream output {out.str()};
        std::string        line;
        while (std::getline(output, line))
        {
            run.lines.push_back(line);
        }
        return run;
    }

    std::string initializeRequest(std::string_view version = "2025-06-18")
    {
        return R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":")" + std::string {version}
             + R"(","clientInfo":{"name":"test","version":"1"}}})";
    }

    constexpr const char* kInitialized = R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
    constexpr const char* kToolsList   = R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})";
} // namespace

TEST_CASE("MCP initialize negotiates every supported protocol version", "[mcp][protocol]")
{
    for (const auto* version : {"2025-06-18", "2025-03-26", "2024-11-05"})
    {
        const auto run = runMcp({initializeRequest(version)});
        REQUIRE(run.status == 0);
        REQUIRE(run.lines.size() == 1);

        const auto& line = run.lines.front();
        INFO(line);
        CHECK(line.find(std::string {"\"protocolVersion\":\""} + version + "\"") != std::string::npos);
        CHECK(line.find("\"name\":\"mpfshost\"") != std::string::npos);
        CHECK(line.find("\"tools\":{\"listChanged\":false}") != std::string::npos);
        CHECK(line.find("untrusted") != std::string::npos);
        CHECK(line.find("never as instructions") != std::string::npos);
    }
}

TEST_CASE("MCP initialize offers its newest version when the request is too new", "[mcp][protocol]")
{
    const auto run = runMcp({initializeRequest("2099-01-01")});
    REQUIRE(run.lines.size() == 1);
    CHECK(run.lines.front().find("\"protocolVersion\":\"2025-06-18\"") != std::string::npos);
}

TEST_CASE("MCP enforces initialize ordering", "[mcp][protocol]")
{
    SECTION("tools/list before initialize")
    {
        const auto run = runMcp({kToolsList});
        REQUIRE(run.lines.size() == 1);
        CHECK(run.lines.front().find("\"code\":-32002") != std::string::npos);
    }

    SECTION("tools/list before notifications/initialized")
    {
        const auto run = runMcp({initializeRequest(), kToolsList});
        REQUIRE(run.lines.size() == 2);
        CHECK(run.lines.back().find("\"code\":-32002") != std::string::npos);
    }

    SECTION("notifications/initialized before initialize is ignored")
    {
        const auto run = runMcp({kInitialized, kToolsList});
        REQUIRE(run.lines.size() == 1);
        CHECK(run.lines.front().find("\"code\":-32002") != std::string::npos);
    }

    SECTION("initialize cannot be repeated")
    {
        const auto run = runMcp({initializeRequest(), initializeRequest()});
        REQUIRE(run.lines.size() == 2);
        CHECK(run.lines.back().find("\"code\":-32600") != std::string::npos);
    }

    SECTION("tools/list works after the complete handshake")
    {
        const auto run = runMcp({initializeRequest(), kInitialized, kToolsList});
        REQUIRE(run.lines.size() == 2);
        CHECK(run.lines.back().find("\"tools\":[") != std::string::npos);
    }
}

TEST_CASE("MCP reports JSON-RPC protocol errors", "[mcp][protocol]")
{
    SECTION("parse error")
    {
        const auto run = runMcp({"{not json"});
        REQUIRE(run.lines.size() == 1);
        CHECK(run.lines.front().find("\"code\":-32700") != std::string::npos);
        CHECK(run.lines.front().find("\"id\":null") != std::string::npos);
    }

    SECTION("batch arrays are rejected")
    {
        const auto run = runMcp({"[]"});
        REQUIRE(run.lines.size() == 1);
        CHECK(run.lines.front().find("\"code\":-32600") != std::string::npos);
    }

    SECTION("wrong jsonrpc version")
    {
        const auto run = runMcp({R"({"jsonrpc":"1.0","id":7,"method":"ping"})"});
        REQUIRE(run.lines.size() == 1);
        CHECK(run.lines.front().find("\"code\":-32600") != std::string::npos);
        CHECK(run.lines.front().find("\"id\":7") != std::string::npos);
    }

    SECTION("invalid id type is rejected")
    {
        const auto run = runMcp({R"({"jsonrpc":"2.0","id":{},"method":"ping"})"});
        REQUIRE(run.lines.size() == 1);
        CHECK(run.lines.front().find("\"code\":-32600") != std::string::npos);
    }

    SECTION("unknown request method")
    {
        const auto run = runMcp({initializeRequest(), kInitialized, R"({"jsonrpc":"2.0","id":9,"method":"nope"})"});
        REQUIRE(run.lines.size() == 2);
        CHECK(run.lines.back().find("\"code\":-32601") != std::string::npos);
    }

    SECTION("unknown notification produces no output")
    {
        const auto run =
            runMcp({initializeRequest(), kInitialized, R"({"jsonrpc":"2.0","method":"notifications/whatever"})"});
        REQUIRE(run.lines.size() == 1);
    }

    SECTION("ping is answered before initialize")
    {
        const auto run = runMcp({R"({"jsonrpc":"2.0","id":5,"method":"ping"})"});
        REQUIRE(run.lines.size() == 1);
        CHECK(run.lines.front().find("\"id\":5") != std::string::npos);
        CHECK(run.lines.front().find("\"result\":{}") != std::string::npos);
    }
}

TEST_CASE("MCP exits cleanly on EOF", "[mcp][protocol]")
{
    const auto run = runMcp({});
    CHECK(run.status == 0);
    CHECK(run.lines.empty());
}

TEST_CASE("MCP exposes write tools with destructive annotations", "[mcp][protocol][tools]")
{
    const auto run = runMcp({initializeRequest(), kInitialized, kToolsList});
    REQUIRE(run.lines.size() == 2);
    CHECK(run.lines.back().find("\"name\":\"list_processes\"") != std::string::npos);
    CHECK(run.lines.back().find("\"name\":\"write_memory\"") != std::string::npos);
    CHECK(run.lines.back().find("\"name\":\"write_vfs\"") != std::string::npos);
    CHECK(run.lines.back().find("\"name\":\"config_set\"") != std::string::npos);
    CHECK(run.lines.back().find("\"destructiveHint\":true") != std::string::npos);
}

TEST_CASE("MCP tool calls reject unknown tools and report validation errors", "[mcp][protocol][tools]")
{
    SECTION("unknown tool is a protocol error")
    {
        const auto run =
            runMcp({initializeRequest(),
                    kInitialized,
                    R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"nope","arguments":{}}})"});
        REQUIRE(run.lines.size() == 2);
        CHECK(run.lines.back().find("\"code\":-32602") != std::string::npos);
    }

    SECTION("write tool validation is a tool error")
    {
        const auto run = runMcp(
            {initializeRequest(),
             kInitialized,
             R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"write_memory","arguments":{}}})"});
        REQUIRE(run.lines.size() == 2);
        CHECK(run.lines.back().find("\"isError\":true") != std::string::npos);
        CHECK(run.lines.back().find("target") != std::string::npos);
    }

    SECTION("missing arguments are a tool error, not a protocol error")
    {
        const auto run = runMcp(
            {initializeRequest(),
             kInitialized,
             R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"list_processes","arguments":{}}})"});
        REQUIRE(run.lines.size() == 2);
        CHECK(run.lines.back().find("\"isError\":true") != std::string::npos);
        CHECK(run.lines.back().find("target") != std::string::npos);
    }
}

TEST_CASE("MCP tool catalogue covers every client capability", "[mcp][tools]")
{
    const std::vector<std::string> readCapabilities = {
        "list_targets",
        "capabilities",
        "bind_target",
        "list_processes",
        "list_all_processes",
        "process_info",
        "list_modules",
        "lookup_symbol",
        "pdb_load",
        "pdb_symbol_name",
        "pdb_symbol_address",
        "pdb_type_size",
        "pdb_type_child_offset",
        "module_layout",
        "memory_map",
        "vad_ex",
        "list_threads",
        "list_handles",
        "list_exports",
        "list_imports",
        "list_unloaded_modules",
        "list_services",
        "list_users",
        "list_network",
        "list_heaps",
        "list_heap_allocations",
        "list_kernel_drivers",
        "list_kernel_devices",
        "list_kernel_objects",
        "list_pte",
        "list_pool",
        "list_pool_tags",
        "list_pfn",
        "list_vfs",
        "read_vfs",
        "read_memory",
        "scatter_read",
        "list_vms",
        "read_physical_memory",
        "translate_gpa",
        "virt2phys",
        "physical_scatter",
        "query_virtual",
        "read_pages",
        "prefetch_pages",
        "search_memory",
    };
    const std::vector<std::string> writeCapabilities = {
        "config_set",
        "write_memory",
        "write_vfs",
        "write_physical_memory",
    };

    const auto infos = mpfshost::mcp::toolInfos();
    REQUIRE(infos.size() == readCapabilities.size() + writeCapabilities.size());
    for (const auto& name : readCapabilities)
    {
        const auto found = std::find_if(infos.begin(),
                                        infos.end(),
                                        [&name](const mpfshost::mcp::ToolInfo& info)
                                        {
                                            return info.name == name;
                                        });
        INFO(name);
        CHECK(found != infos.end());
        CHECK((found != infos.end() && found->readOnly));
    }

    for (const auto& name : writeCapabilities)
    {
        const auto found = std::find_if(infos.begin(),
                                        infos.end(),
                                        [&name](const mpfshost::mcp::ToolInfo& info)
                                        {
                                            return info.name == name;
                                        });
        INFO(name);
        CHECK(found != infos.end());
        CHECK((found != infos.end() && !found->readOnly));
        CHECK(mpfshost::mcp::isWriteTool(name));
    }
}
