#ifndef MPFSHOST_CORE_SERVER_KEYBOARD_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_KEYBOARD_PROVIDER_HPP
#pragma once

#include "core/ipc/message.hpp"

#include <optional>
#include <string>

namespace mpfshost
{
    // Reads the guest's Windows async key-state bitmap. `unavailable` reports a target that cannot expose it
    // (not Windows, or a build older than Windows 11 22H2), which the session maps to CapabilityUnavailable.
    class KeyboardProvider
    {
    public:
        virtual ~KeyboardProvider() = default;

        [[nodiscard]] virtual std::optional<KeyboardStateResponse>
        keyboardState(const KeyboardStateRequest& request, std::string& error, bool& unavailable) = 0;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_KEYBOARD_PROVIDER_HPP
