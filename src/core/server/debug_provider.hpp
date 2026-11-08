#ifndef MPFSHOST_CORE_SERVER_DEBUG_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_DEBUG_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"

#include <optional>
#include <string>

namespace mpfshost
{
    // Guest control through QEMU's QMP socket. Both calls answer the complete debug status, so a control
    // request does not need a second round trip to report the resulting run state.
    class DebugProvider
    {
    public:
        virtual ~DebugProvider() = default;

        [[nodiscard]] virtual std::optional<DebugStatus> debugState(std::string& error)         = 0;
        [[nodiscard]] virtual std::optional<DebugStatus> debugControl(DebugControlAction action,
                                                                      std::string&       error) = 0;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_DEBUG_PROVIDER_HPP
