#ifndef MPFSHOST_HOST_VMM_DEBUG_HPP
#define MPFSHOST_HOST_VMM_DEBUG_HPP

#pragma once

#include "core/ipc/message.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace mpfshost
{
    namespace debug
    {
        class QmpClient;
    }

    // One controller per VMM handle, protected by its own mutex because sessions share the handle. The
    // controller owns the host-side debug bookkeeping; QEMU's QMP socket is the source of truth for the
    // guest's actual run state.
    class DebugController
    {
    public:
        explicit DebugController(std::filesystem::path qmpSocket);

        DebugController(const DebugController&)            = delete;
        DebugController& operator=(const DebugController&) = delete;

        [[nodiscard]] std::optional<DebugStatus> state(std::string& error);
        [[nodiscard]] std::optional<DebugStatus> control(DebugControlAction action, std::string& error);

    private:
        [[nodiscard]] std::optional<DebugStatus> readStatus(debug::QmpClient& client, std::string& error);

        std::mutex            _mutex;
        std::filesystem::path _qmpSocket;
        bool                  _stubActive {false};
        bool                  _hostPaused {false};
        std::uint32_t         _watchCount {0};
    };
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_DEBUG_HPP
