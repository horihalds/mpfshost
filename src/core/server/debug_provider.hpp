#ifndef MPFSHOST_CORE_SERVER_DEBUG_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_DEBUG_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    using DebugWatchHitSink    = std::function<bool(const DebugWatchData&)>;
    using DebugWatchAbortCheck = std::function<bool()>;

    // Guest control through QEMU's QMP socket. Every call answers the complete debug status, so a control
    // request does not need a second round trip to report the resulting run state.
    class DebugProvider
    {
    public:
        virtual ~DebugProvider() = default;

        // `busy` reports another debug operation holding the target, which the session maps to ErrorCode::Busy.
        [[nodiscard]] virtual std::optional<DebugStatus> debugState(bool& busy, std::string& error) = 0;
        [[nodiscard]] virtual std::optional<DebugStatus>
        debugControl(DebugControlAction action, bool& busy, std::string& error) = 0;
        // `invalidState` reports a running guest or another owner's stub, which the session maps to
        // ErrorCode::InvalidState instead of Internal.
        [[nodiscard]] virtual std::optional<std::vector<DebugRegisterRecord>>
        debugRegisters(const DebugRegistersRequest& request, bool& invalidState, bool& busy, std::string& error) = 0;
        // `onHit` sends one data frame and returns false when it could not be written; `aborted` is polled
        // between short stops so cancellation, the request deadline and peer loss all end the watch quickly.
        [[nodiscard]] virtual std::optional<DebugWatchEnd> debugWatch(const DebugWatchRequest&    request,
                                                                      const DebugWatchHitSink&    onHit,
                                                                      const DebugWatchAbortCheck& aborted,
                                                                      bool&                       busy,
                                                                      std::string&                error)         = 0;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_DEBUG_PROVIDER_HPP
