#include "host/vmm_debug.hpp"

#include "core/debug/qmp_client.hpp"
#include "core/log/log.hpp"
#include "host/vmm_handle.hpp"

#include <chrono>
#include <utility>

namespace mpfshost
{
    namespace
    {
        constexpr auto kQmpTimeout = std::chrono::seconds {5};
    } // namespace

    DebugController::DebugController(std::filesystem::path qmpSocket) : _qmpSocket {std::move(qmpSocket)} {}

    std::optional<DebugStatus> DebugController::state(std::string& error)
    {
        std::lock_guard lock {_mutex};

        debug::QmpClient client {debug::makeUnixQmpTransport()};
        if (!client.connect(_qmpSocket, kQmpTimeout, error))
        {
            return std::nullopt;
        }
        return readStatus(client, error);
    }

    std::optional<DebugStatus> DebugController::control(DebugControlAction action, std::string& error)
    {
        std::lock_guard lock {_mutex};

        debug::QmpClient client {debug::makeUnixQmpTransport()};
        if (!client.connect(_qmpSocket, kQmpTimeout, error))
        {
            return std::nullopt;
        }

        debug::QmpStatusInfo current;
        if (!client.queryStatus(current, kQmpTimeout, error))
        {
            return std::nullopt;
        }

        const auto before = debug::qmpRunState(current.status);
        if (action == DebugControlAction::Stop)
        {
            if (before == DebugRunState::Running)
            {
                std::string ignored;
                if (!client.execute("stop", ignored, kQmpTimeout, error))
                {
                    return std::nullopt;
                }
                _hostPaused = true;
                log::info("host", "paused the guest through QMP");
            }
        }
        else
        {
            if (before != DebugRunState::Running)
            {
                std::string ignored;
                if (!client.execute("cont", ignored, kQmpTimeout, error))
                {
                    return std::nullopt;
                }
                if (_hostPaused)
                {
                    log::info("host", "resumed the guest through QMP");
                }
            }
            _hostPaused = false;
        }

        return readStatus(client, error);
    }

    std::optional<DebugStatus> DebugController::readStatus(debug::QmpClient& client, std::string& error)
    {
        debug::QmpStatusInfo info;
        if (!client.queryStatus(info, kQmpTimeout, error))
        {
            return std::nullopt;
        }

        std::uint32_t vcpuCount = 0;
        if (!client.queryVcpuCount(vcpuCount, kQmpTimeout, error))
        {
            return std::nullopt;
        }

        DebugStatus status;
        status.runState   = debug::qmpRunState(info.status);
        status.stubActive = _stubActive;
        status.hostPaused = _hostPaused;
        status.vcpuCount  = vcpuCount;
        status.watchCount = _watchCount;
        return status;
    }

    std::optional<DebugStatus> VmmHandle::debugState(std::string& error)
    {
        return _debug.state(error);
    }

    std::optional<DebugStatus> VmmHandle::debugControl(DebugControlAction action, std::string& error)
    {
        return _debug.control(action, error);
    }
} // namespace mpfshost
