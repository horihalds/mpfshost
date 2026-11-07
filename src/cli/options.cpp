#include "pch.hpp"

#include "cli/options.hpp"
#include "cli/usage.hpp"

#include "core/log/log.hpp"
#include "core/util/hex.hpp"
#include "core/util/number.hpp"
#include "core/version.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <grp.h>
#include <pwd.h>

namespace mpfshost::cli
{
    namespace
    {
        bool parseOctalMode(std::string_view text, std::uint32_t& mode)
        {
            if (text.empty())
            {
                return false;
            }
            std::uint32_t parsed {};
            const auto    result = std::from_chars(text.data(), text.data() + text.size(), parsed, 8);
            if (result.ec != std::errc {} || result.ptr != text.data() + text.size() || parsed > 0777)
            {
                return false;
            }
            mode = parsed;
            return true;
        }

        std::optional<std::uint32_t> resolveUserId(std::string_view text)
        {
            if (const auto uid = mpfshost::parseDecimalU32(text))
            {
                return uid;
            }
            const std::string name {text};
            const passwd*     entry = ::getpwnam(name.c_str());
            if (entry == nullptr)
            {
                return std::nullopt;
            }
            return static_cast<std::uint32_t>(entry->pw_uid);
        }
    } // namespace

    std::optional<std::uint32_t> resolveGroupId(std::string_view text)
    {
        if (const auto gid = mpfshost::parseDecimalU32(text))
        {
            return gid;
        }
        const std::string name {text};
        const group*      entry = ::getgrnam(name.c_str());
        if (entry == nullptr)
        {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(entry->gr_gid);
    }

    ParseOutcome parseCommandLine(int argc, char* const argv[], CommandLine& commandLine)
    {
        std::vector<std::string_view> clientArgs;

        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument {argv[index]};
            if (argument == "--version")
            {
                std::printf("%s\n", std::string {mpfshost::version()}.c_str());
                return ParseOutcome::ExitSuccess;
            }
            if (argument == "--help")
            {
                printUsage();
                return ParseOutcome::ExitSuccess;
            }
            if (argument == "--socket")
            {
                if (index + 1 >= argc)
                {
                    mpfshost::log::error("client", "--socket needs a path");
                    return ParseOutcome::ExitFailure;
                }
                commandLine.server.socketPath = argv[++index];
                continue;
            }
            if (argument == "--log-level")
            {
                if (index + 1 >= argc)
                {
                    mpfshost::log::error("client", "--log-level needs a value");
                    return ParseOutcome::ExitFailure;
                }
                const std::string_view levelText {argv[++index]};
                const auto             parsed = mpfshost::log::parseLevel(levelText);
                if (!parsed.has_value())
                {
                    mpfshost::log::error(
                        "client", "unknown log level '", levelText, "'; expected error, warn, info or debug");
                    return ParseOutcome::ExitFailure;
                }
                mpfshost::log::setLevel(*parsed);
                continue;
            }
            if (argument == "--handle-idle-timeout")
            {
                if (index + 1 >= argc)
                {
                    mpfshost::log::error("client", "--handle-idle-timeout needs a value");
                    return ParseOutcome::ExitFailure;
                }
                const auto parsed = mpfshost::parseDecimalU32(argv[++index]);
                if (!parsed)
                {
                    mpfshost::log::error("client", "--handle-idle-timeout needs a non-negative integer");
                    return ParseOutcome::ExitFailure;
                }
                commandLine.server.handleIdleSeconds = *parsed;
                continue;
            }
            if (argument == "--workers")
            {
                if (index + 1 >= argc)
                {
                    mpfshost::log::error("client", "--workers needs a value");
                    return ParseOutcome::ExitFailure;
                }
                const auto parsed = mpfshost::parseDecimalU32(argv[++index]);
                if (!parsed || *parsed == 0)
                {
                    mpfshost::log::error("client", "--workers needs a positive integer");
                    return ParseOutcome::ExitFailure;
                }
                commandLine.server.workers = *parsed;
                continue;
            }
            if (argument == "--pending-connections")
            {
                if (index + 1 >= argc)
                {
                    mpfshost::log::error("client", "--pending-connections needs a value");
                    return ParseOutcome::ExitFailure;
                }
                const auto parsed = mpfshost::parseDecimalU32(argv[++index]);
                if (!parsed)
                {
                    mpfshost::log::error("client", "--pending-connections needs a non-negative integer");
                    return ParseOutcome::ExitFailure;
                }
                commandLine.server.pendingConnections = *parsed;
                continue;
            }
            if (argument == "--request-timeout")
            {
                if (index + 1 >= argc)
                {
                    mpfshost::log::error("client", "--request-timeout needs a value");
                    return ParseOutcome::ExitFailure;
                }
                const auto parsed = mpfshost::parseDecimalU32(argv[++index]);
                if (!parsed)
                {
                    mpfshost::log::error("client", "--request-timeout needs a non-negative integer");
                    return ParseOutcome::ExitFailure;
                }
                commandLine.server.requestTimeoutSeconds = *parsed;
                continue;
            }
            if (argument == "--idle-timeout")
            {
                if (index + 1 >= argc)
                {
                    mpfshost::log::error("client", "--idle-timeout needs a value");
                    return ParseOutcome::ExitFailure;
                }
                const auto parsed = mpfshost::parseDecimalU32(argv[++index]);
                if (!parsed)
                {
                    mpfshost::log::error("client", "--idle-timeout needs a non-negative integer");
                    return ParseOutcome::ExitFailure;
                }
                commandLine.server.idleTimeoutSeconds = *parsed;
                continue;
            }
            if (argument == "--allow-all")
            {
                commandLine.server.allowAll = true;
                continue;
            }
            if (argument == "--allow-vfs-write")
            {
                commandLine.server.allowVfsWrite = true;
                continue;
            }
            if (argument == "--allow-uid")
            {
                if (index + 1 >= argc)
                {
                    mpfshost::log::error("client", "--allow-uid needs a uid");
                    return ParseOutcome::ExitFailure;
                }
                const auto uid = mpfshost::parseDecimalU32(argv[++index]);
                if (!uid)
                {
                    mpfshost::log::error("client", "--allow-uid needs a non-negative integer");
                    return ParseOutcome::ExitFailure;
                }
                commandLine.server.access.allowUid(*uid);
                continue;
            }
            if (argument == "--allow-user")
            {
                if (index + 1 >= argc)
                {
                    mpfshost::log::error("client", "--allow-user needs a user name or uid");
                    return ParseOutcome::ExitFailure;
                }
                const std::string_view name {argv[++index]};
                const auto             uid = resolveUserId(name);
                if (!uid)
                {
                    mpfshost::log::error("client", "unknown user '", name, "'");
                    return ParseOutcome::ExitFailure;
                }
                commandLine.server.access.allowUid(*uid);
                continue;
            }
            if (argument == "--allow-gid")
            {
                if (index + 1 >= argc)
                {
                    mpfshost::log::error("client", "--allow-gid needs a gid");
                    return ParseOutcome::ExitFailure;
                }
                const auto gid = mpfshost::parseDecimalU32(argv[++index]);
                if (!gid)
                {
                    mpfshost::log::error("client", "--allow-gid needs a non-negative integer");
                    return ParseOutcome::ExitFailure;
                }
                commandLine.server.access.allowGid(*gid);
                continue;
            }
            if (argument == "--allow-group")
            {
                if (index + 1 >= argc)
                {
                    mpfshost::log::error("client", "--allow-group needs a group name or gid");
                    return ParseOutcome::ExitFailure;
                }
                const std::string_view name {argv[++index]};
                const auto             gid = resolveGroupId(name);
                if (!gid)
                {
                    mpfshost::log::error("client", "unknown group '", name, "'");
                    return ParseOutcome::ExitFailure;
                }
                commandLine.server.access.allowGid(*gid);
                continue;
            }
            if (argument == "--socket-group")
            {
                if (index + 1 >= argc)
                {
                    mpfshost::log::error("client", "--socket-group needs a group name or gid");
                    return ParseOutcome::ExitFailure;
                }
                const std::string_view name {argv[++index]};
                const auto             gid = resolveGroupId(name);
                if (!gid)
                {
                    mpfshost::log::error("client", "unknown group '", name, "'");
                    return ParseOutcome::ExitFailure;
                }
                commandLine.server.socketGid = *gid;
                continue;
            }
            if (argument == "--socket-mode")
            {
                if (index + 1 >= argc)
                {
                    mpfshost::log::error("client", "--socket-mode needs an octal mode");
                    return ParseOutcome::ExitFailure;
                }
                if (!parseOctalMode(argv[++index], commandLine.server.socketMode))
                {
                    mpfshost::log::error("client", "--socket-mode needs an octal mode between 0000 and 0777");
                    return ParseOutcome::ExitFailure;
                }
                commandLine.server.socketModeSet = true;
                continue;
            }

            // Every token the server flags above did not consume belongs to the client half of the command
            // line, which core parses. Its own flags and their values are taken here first, so a client value
            // that happens to look like a server flag is still stolen by this loop; that only affects nonsense
            // command lines and is cheaper than keeping a second copy of the arity table down here.
            clientArgs.push_back(argument);
        }

        if (clientArgs.empty())
        {
            return ParseOutcome::Continue;
        }

        std::string error;
        auto        client = mpfshost::parseClientOptions(clientArgs, error);
        if (!client)
        {
            // An empty error means the tokens held no --client, so the command line is not a client invocation.
            if (!error.empty())
            {
                mpfshost::log::error("client", error);
                return ParseOutcome::ExitFailure;
            }
            return ParseOutcome::Continue;
        }
        client->socketPath = commandLine.server.socketPath;
        commandLine.client = std::move(*client);
        return ParseOutcome::Continue;
    }
} // namespace mpfshost::cli
