#include "pch.hpp"

#include "cli/options.hpp"
#include "cli/run_client.hpp"
#include "core/log/log.hpp"
#include "core/server/handle_cache.hpp"
#include "core/server/server.hpp"
#include "core/version.hpp"
#include "host/filesystem_target_source.hpp"
#include "host/vmm_handle_factory.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <unistd.h>

namespace
{
    std::string socketModeText(std::uint32_t mode)
    {
        char buffer[8] {};
        std::snprintf(buffer, sizeof(buffer), "%04o", static_cast<unsigned>(mode));
        return buffer;
    }

    mpfshost::Server*          g_server = nullptr;
    volatile std::sig_atomic_t g_signal = 0;

    void handleSignal(int signal)
    {
        g_signal = signal;
        if (g_server != nullptr)
        {
            g_server->requestStop();
        }
    }

    std::string_view signalName(int signal)
    {
        switch (signal)
        {
        case SIGINT:
            return "SIGINT";
        case SIGTERM:
            return "SIGTERM";
        default:
            return "signal";
        }
    }

    mpfshost::HandleCacheOptions handleCacheOptions(std::uint32_t idleSeconds)
    {
        mpfshost::HandleCacheOptions options;
        options.idleTimeout = std::chrono::seconds {idleSeconds};
        if (idleSeconds == 0)
        {
            options.sweepInterval = std::chrono::milliseconds {0};
            return options;
        }

        const auto quarter =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::seconds {idleSeconds}) / 4;
        options.sweepInterval =
            std::clamp(quarter, std::chrono::milliseconds {1'000}, std::chrono::milliseconds {30'000});
        return options;
    }

    int runServer(mpfshost::ServerOptions serverOptions, std::uint32_t handleIdleSeconds)
    {
        const auto& socketPath = serverOptions.socketPath;

        if (::geteuid() != 0)
        {
            mpfshost::log::error("server", "the host needs root; start it with sudo");
            return 1;
        }

        // Declaration order matters: Server joins every session thread before the cache and the factory are
        // destroyed, so no handle is closed under a running session or outlives VMMDLL_CloseAll().
        mpfshost::FilesystemTargetSource targets;
        mpfshost::VmmHandleFactory       factory;
        mpfshost::CachingBackend         backend {factory, handleCacheOptions(handleIdleSeconds)};

        const auto discovered = targets.scan();
        mpfshost::log::info("server",
                            "mpfshost ",
                            mpfshost::version(),
                            " pid=",
                            static_cast<int>(::getpid()),
                            " euid=",
                            static_cast<unsigned>(::geteuid()));
        std::string socketDescription =
            "socket " + socketPath.string() + " (mode " + socketModeText(serverOptions.socketMode);
        if (serverOptions.socketGid)
        {
            socketDescription += ", group " + std::to_string(*serverOptions.socketGid);
        }
        socketDescription += ')';
        mpfshost::log::info("server", socketDescription);
        if (handleIdleSeconds == 0)
        {
            mpfshost::log::info("server", "handle idle timeout disabled; handles are kept until shutdown");
        }
        else
        {
            mpfshost::log::info("server", "handle idle timeout ", handleIdleSeconds, "s");
        }
        if (serverOptions.allowVfsWrite)
        {
            mpfshost::log::warn("server", "vfs write is enabled; clients may modify guest files");
        }
        mpfshost::log::info("server", "discovered ", discovered.size(), " VM target(s)");
        for (const auto& target : discovered)
        {
            mpfshost::log::debug("server",
                                 "target ",
                                 target.name,
                                 " qemu-pid=",
                                 static_cast<unsigned>(target.qemuPid),
                                 " qmp=",
                                 target.qmpSocket.string());
        }

        mpfshost::Server server {serverOptions, targets, backend};

        g_server = &server;
        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);

        std::string error;
        if (!server.run(error))
        {
            mpfshost::log::error("server", error);
            g_server = nullptr;
            return 1;
        }
        g_server = nullptr;

        std::error_code filesystemError;
        std::filesystem::remove(socketPath, filesystemError);
        if (g_signal != 0)
        {
            mpfshost::log::info("server", "stopping on ", signalName(g_signal), "; removed ", socketPath.string());
        }
        else
        {
            mpfshost::log::info("server", "stopped; removed ", socketPath.string());
        }
        return 0;
    }
} // namespace

int main(int argc, char* argv[])
{
    if (const char* environmentLevel = std::getenv("MPFSHOST_LOG_LEVEL"); environmentLevel != nullptr)
    {
        if (const auto parsed = mpfshost::log::parseLevel(environmentLevel); parsed.has_value())
        {
            mpfshost::log::setLevel(*parsed);
        }
        else
        {
            mpfshost::log::setLevel(mpfshost::log::Level::Info);
            mpfshost::log::warn("main",
                                "ignoring invalid MPFSHOST_LOG_LEVEL '",
                                environmentLevel,
                                "'; expected error, warn, info or debug");
        }
    }

    mpfshost::cli::CommandLine commandLine;
    switch (mpfshost::cli::parseCommandLine(argc, argv, commandLine))
    {
    case mpfshost::cli::ParseOutcome::ExitSuccess:
        return 0;
    case mpfshost::cli::ParseOutcome::ExitFailure:
        return 1;
    case mpfshost::cli::ParseOutcome::Continue:
        break;
    }

    if (commandLine.client)
    {
        return mpfshost::cli::runClient(*commandLine.client);
    }

    auto& accessPolicy = commandLine.server.access;
    if (commandLine.server.allowAll && accessPolicy.enforcing())
    {
        mpfshost::log::error("server",
                             "--allow-all cannot be combined with --allow-uid/--allow-user/--allow-gid/--allow-group");
        return 1;
    }

    if (!commandLine.server.allowAll)
    {
        const auto gid = mpfshost::cli::resolveGroupId(mpfshost::cli::kDefaultAccessGroup);
        if (!gid)
        {
            mpfshost::log::error("server",
                                 "access group '",
                                 mpfshost::cli::kDefaultAccessGroup,
                                 "' does not exist; create it with 'sudo groupadd --system ",
                                 mpfshost::cli::kDefaultAccessGroup,
                                 "' or start with --allow-all to serve every local user");
            return 1;
        }
        accessPolicy.allowGid(*gid, mpfshost::cli::kDefaultAccessGroup);
        if (!commandLine.server.socketModeSet)
        {
            commandLine.server.socketMode = 0660;
        }
        if (!commandLine.server.socketGid)
        {
            commandLine.server.socketGid = *gid;
        }
    }

    mpfshost::ServerOptions serverOptions {commandLine.server.socketPath};
    serverOptions.access             = accessPolicy;
    serverOptions.socketMode         = commandLine.server.socketMode;
    serverOptions.socketGid          = commandLine.server.socketGid;
    serverOptions.workers            = commandLine.server.workers;
    serverOptions.pendingConnections = commandLine.server.pendingConnections;
    serverOptions.requestTimeout     = std::chrono::seconds {commandLine.server.requestTimeoutSeconds};
    serverOptions.idleTimeout        = std::chrono::seconds {commandLine.server.idleTimeoutSeconds};
    serverOptions.allowVfsWrite      = commandLine.server.allowVfsWrite;
    return runServer(std::move(serverOptions), commandLine.server.handleIdleSeconds);
}
