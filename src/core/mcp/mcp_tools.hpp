#ifndef MPFSHOST_CORE_MCP_MCP_TOOLS_HPP
#define MPFSHOST_CORE_MCP_MCP_TOOLS_HPP
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mpfshost::mcp
{
    struct ToolInfo
    {
        std::string name;
        bool        readOnly {};
    };

    // Tools exposed to the model. Write tools are always present.
    [[nodiscard]] std::vector<ToolInfo> toolInfos();

    [[nodiscard]] bool isWriteTool(std::string_view name);

    // The JSON array of MCP tool descriptors, already annotated for the model.
    [[nodiscard]] std::string toolListJson();

    struct ToolContext
    {
        std::filesystem::path socketPath;
    };

    struct ToolOutcome
    {
        bool        isError {};
        std::string structuredJson; // empty on an error result
        std::string text;           // one MCP text content item
    };

    // `argumentsJson` is the raw JSON object from tools/call params.arguments. Validation and host failures are
    // reported through the outcome with isError set; the JSON-RPC layer only sees protocol errors.
    [[nodiscard]] ToolOutcome
    callTool(std::string_view name, std::string_view argumentsJson, const ToolContext& context);
} // namespace mpfshost::mcp

#endif // MPFSHOST_CORE_MCP_MCP_TOOLS_HPP
