#ifndef MPFSHOST_CORE_MCP_MCP_SERVER_HPP
#define MPFSHOST_CORE_MCP_MCP_SERVER_HPP
#pragma once

#include "core/mcp/mcp_tools.hpp"

#include <filesystem>
#include <istream>
#include <ostream>
#include <string>

namespace mpfshost::mcp
{
    struct ServerOptions
    {
        std::filesystem::path socketPath;
        bool                  allowWrite {};
    };

    // One JSON-RPC 2.0 message per line over the supplied streams. The CLI passes std::cin and std::cout; tests
    // pass in-memory streams and drive the same object in-process.
    class McpServer
    {
    public:
        McpServer(std::istream& input, std::ostream& output, ServerOptions options);

        McpServer(const McpServer&)            = delete;
        McpServer& operator=(const McpServer&) = delete;

        // Reads until EOF. Always returns 0; transport setup happens before this object is created.
        int run();

    private:
        void processLine(std::string line);
        void emit(const std::string& payload);

        std::istream& _input;
        std::ostream& _output;
        ServerOptions _options;
        bool          _initializeSeen {};
        bool          _ready {};
    };
} // namespace mpfshost::mcp

#endif // MPFSHOST_CORE_MCP_MCP_SERVER_HPP
