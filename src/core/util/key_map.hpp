#ifndef MPFSHOST_CORE_UTIL_KEY_MAP_HPP
#define MPFSHOST_CORE_UTIL_KEY_MAP_HPP
#pragma once

#include "core/ipc/message.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mpfshost
{
    // Resolves a user-facing key name to a QEMU qcode. Accepts the qcode names themselves plus a few aliases such
    // as `enter` and `escape`. Returns nothing for an unknown name.
    [[nodiscard]] std::optional<std::string> inputKeyCode(std::string_view name);

    // Expands printable ASCII text into key events for a US layout, using `shift` for uppercase and shifted
    // symbols. Non-ASCII input is rejected instead of being guessed.
    [[nodiscard]] bool appendTextInput(std::string_view text, std::vector<InputEvent>& events, std::string& error);

    [[nodiscard]] std::optional<InputMouseButton> inputMouseButton(std::string_view name);
    [[nodiscard]] std::string_view                inputMouseButtonName(InputMouseButton button) noexcept;
} // namespace mpfshost

#endif // MPFSHOST_CORE_UTIL_KEY_MAP_HPP
