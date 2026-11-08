#ifndef MPFSHOST_CORE_DEBUG_QMP_CLIENT_HPP
#define MPFSHOST_CORE_DEBUG_QMP_CLIENT_HPP
#pragma once

#include "core/ipc/message.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace mpfshost::debug
{
    inline constexpr std::size_t kMaxQmpLineLength = 1u << 20;

    // One newline-delimited JSON message per call. Implementations own the socket; the client only frames.
    class QmpTransport
    {
    public:
        virtual ~QmpTransport() = default;

        [[nodiscard]] virtual bool
        open(const std::filesystem::path& socketPath, std::chrono::milliseconds timeout, std::string& error) = 0;
        [[nodiscard]] virtual bool
        writeLine(std::string_view line, std::chrono::milliseconds timeout, std::string& error) = 0;
        // `closed` distinguishes a peer EOF from a timeout; both report false.
        [[nodiscard]] virtual bool
        readLine(std::string& line, std::chrono::milliseconds timeout, bool& closed, std::string& error) = 0;
        virtual void close() noexcept                                                                    = 0;
    };

    [[nodiscard]] std::unique_ptr<QmpTransport> makeUnixQmpTransport();

    struct QmpStatusInfo
    {
        std::string status; // QEMU's raw query-status string
        bool        running {};
        bool        singlestep {};
    };

    // Speaks the QMP control channel: greeting, qmp_capabilities, command/reply matching and asynchronous
    // event skipping. Every wait is bounded by the caller's timeout.
    class QmpClient
    {
    public:
        explicit QmpClient(std::unique_ptr<QmpTransport> transport);
        ~QmpClient();

        QmpClient(const QmpClient&)            = delete;
        QmpClient& operator=(const QmpClient&) = delete;

        [[nodiscard]] bool
        connect(const std::filesystem::path& socketPath, std::chrono::milliseconds timeout, std::string& error);
        // Sends `command` with no arguments and returns the JSON of its `return` member.
        [[nodiscard]] bool execute(std::string_view          command,
                                   std::string&              resultJson,
                                   std::chrono::milliseconds timeout,
                                   std::string&              error);
        [[nodiscard]] bool queryStatus(QmpStatusInfo& info, std::chrono::milliseconds timeout, std::string& error);
        [[nodiscard]] bool queryVcpuCount(std::uint32_t& count, std::chrono::milliseconds timeout, std::string& error);

    private:
        [[nodiscard]] bool waitForReply(std::string& resultJson, std::chrono::milliseconds timeout, std::string& error);

        std::unique_ptr<QmpTransport> _transport;
        bool                          _open {false};
    };

    [[nodiscard]] DebugRunState qmpRunState(std::string_view status) noexcept;
} // namespace mpfshost::debug

#endif // MPFSHOST_CORE_DEBUG_QMP_CLIENT_HPP
