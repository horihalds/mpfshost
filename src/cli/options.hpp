#ifndef MPFSHOST_CLI_OPTIONS_HPP
#define MPFSHOST_CLI_OPTIONS_HPP
#pragma once

#include "core/client/cli_options.hpp"
#include "core/server/access_policy.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mpfshost::cli
{
    inline constexpr std::string_view kDefaultSocketPath  = "/run/mpfshost.sock";
    inline constexpr std::string_view kDefaultAccessGroup = "mpfshost";

    // Owned by core so the parser and its tests can reach it; kept in this namespace for runClient.
    using mpfshost::ClientOptions;

    // The CLI level of the server settings, before the defaults that depend on the access policy are resolved.
    struct ServerOptions
    {
        std::filesystem::path        socketPath {kDefaultSocketPath};
        std::uint32_t                handleIdleSeconds {60};
        std::uint32_t                workers {8};
        std::uint32_t                pendingConnections {64};
        std::uint32_t                requestTimeoutSeconds {30};
        std::uint32_t                idleTimeoutSeconds {300};
        std::uint32_t                socketMode {0666};
        bool                         socketModeSet {};
        std::optional<std::uint32_t> socketGid;
        mpfshost::AccessPolicy       access;
        bool                         allowAll {};
        bool                         allowVfsWrite {};
    };

    struct CommandLine
    {
        ServerOptions                server;
        std::optional<ClientOptions> client; // Set when --client was given.
    };

    // Continue: the whole command line was accepted. ExitSuccess: --help or --version already printed its output.
    // ExitFailure: the offending argument was reported through the logger.
    enum class ParseOutcome
    {
        Continue,
        ExitSuccess,
        ExitFailure,
    };

    [[nodiscard]] ParseOutcome parseCommandLine(int argc, char* const argv[], CommandLine& commandLine);

    [[nodiscard]] std::optional<std::uint32_t> resolveGroupId(std::string_view text);
} // namespace mpfshost::cli

#endif // MPFSHOST_CLI_OPTIONS_HPP
