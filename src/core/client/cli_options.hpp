#ifndef MPFSHOST_CORE_CLIENT_CLI_OPTIONS_HPP
#define MPFSHOST_CORE_CLIENT_CLI_OPTIONS_HPP

#pragma once

#include "core/client/client.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mpfshost
{
    // Mirrors one `--client` flag per field; filled by parseClientOptions and consumed by runClient.
    struct ClientOptions
    {
        std::filesystem::path               socketPath; // filled by the CLI layer from --socket, not by the parser
        std::string                         command;
        std::string                         target;
        std::uint32_t                       pid {};
        std::string                         nameFilter;
        std::string                         moduleName;
        std::string                         symbolName;
        std::string                         typeName;
        std::string                         childName;
        std::uint64_t                       address {};
        std::uint64_t                       dtb {};
        std::uint32_t                       size {};
        std::vector<std::byte>              data;
        std::vector<mpfshost::ScatterRange> ranges;
        std::uint32_t                       repeat {1};
        bool                                clear {};
        std::uint16_t                       moduleFlags {};
        bool                                identifyModules {};
        std::uint32_t                       tid {};
        bool                                callstack {};
        bool                                addressSet {};
        bool                                dtbSet {};
        bool                                sizeSet {};
        bool                                dataSet {};
        std::uint32_t                       vmIndex {};
        std::uint32_t                       vcpu {};
        bool                                vcpuSet {};
        std::uint32_t                       watchWidth {4};
        DebugWatchAccess                    watchAccess {DebugWatchAccess::Write};
        DebugBreakpointKind                 breakpointKind {DebugBreakpointKind::Software};
        std::uint32_t                       maxHits {1};
        std::uint32_t                       watchTimeoutSeconds {30};
        bool                                watchWidthSet {};
        bool                                watchAccessSet {};
        bool                                breakpointKindSet {};
        bool                                maxHitsSet {};
        bool                                watchTimeoutSet {};
        std::string                         path {"\\"};
        bool                                pathSet {};
        std::uint64_t                       offset {};
        std::string                         pattern;
        bool                                hexPattern {};
        std::uint64_t                       minAddress {};
        std::uint64_t                       maxAddress {};
        std::uint32_t                       alignBytes {1};
        std::uint32_t                       maxResults {1024};
        std::uint64_t                       heap {};
        std::string                         heapText;
        bool                                heapSet {};
        std::string                         tagFilter;
        std::uint32_t                       minBytes {};
        bool                                bigPool {};
        std::uint32_t                       pfnStart {};
        std::uint32_t                       pfnCount {1};
        std::uint32_t                       pageOffset {};
        std::uint32_t                       pageCount {1};
        ConfigOption                        configOption {};
        bool                                configOptionSet {};
        std::uint64_t                       value {};
        bool                                valueSet {};
    };

    // `args` is the ordered subsequence of the command line that the server-flag loop did not consume,
    // program name excluded. Returns nullopt with an empty `error` when `args` holds no `--client`, so a
    // caller can tell "not a client invocation" from a real failure. `socketPath` is left empty.
    [[nodiscard]] std::optional<ClientOptions> parseClientOptions(std::span<const std::string_view> args,
                                                                  std::string&                      error);
} // namespace mpfshost

#endif // MPFSHOST_CORE_CLIENT_CLI_OPTIONS_HPP
