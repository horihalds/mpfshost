#ifndef MPFSHOST_CORE_UTIL_KEYBOARD_STATE_HPP
#define MPFSHOST_CORE_UTIL_KEYBOARD_STATE_HPP
#pragma once

#include "core/ipc/message.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace mpfshost
{
    // `gafAsyncKeyState` packs two bits per virtual key: bit 2n is the current down state of key 4b+n.
    [[nodiscard]] std::vector<std::uint8_t> decodeKeyboardState(std::span<const std::byte> state);

    // A short, stable name for a Windows virtual-key code. Returns an empty view for unmapped codes.
    [[nodiscard]] std::string_view virtualKeyName(std::uint8_t virtualKey) noexcept;
} // namespace mpfshost

#endif // MPFSHOST_CORE_UTIL_KEYBOARD_STATE_HPP
