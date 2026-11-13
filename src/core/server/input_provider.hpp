#ifndef MPFSHOST_CORE_SERVER_INPUT_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_INPUT_PROVIDER_HPP
#pragma once

#include "core/ipc/message.hpp"

#include <optional>
#include <string>

namespace mpfshost
{
    // Guest keyboard and pointer input through QEMU's input core (`input-send-event`). `unavailable` reports that
    // the QMP channel could not be reached or does not implement the command, which the session maps to
    // CapabilityUnavailable. Input never needs a paused guest and never starts a debugger stub.
    class InputProvider
    {
    public:
        virtual ~InputProvider() = default;

        [[nodiscard]] virtual std::optional<InputEventResponse>
        input(const InputEventRequest& request, std::string& error, bool& unavailable) = 0;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_INPUT_PROVIDER_HPP
