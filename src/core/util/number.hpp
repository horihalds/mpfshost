#ifndef MPFSHOST_CORE_UTIL_NUMBER_HPP
#define MPFSHOST_CORE_UTIL_NUMBER_HPP

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace mpfshost
{
    // Rejects an empty string, a sign, trailing characters and every value above uint32_t.
    [[nodiscard]] std::optional<std::uint32_t> parseDecimalU32(std::string_view text);

    // A range size is decimal unless written with a 0x prefix, which is how the documented sizes (0x1000 and up)
    // stay unambiguous next to the hex address they follow.
    [[nodiscard]] std::optional<std::uint32_t> parseRangeSize(std::string_view text);
} // namespace mpfshost

#endif // MPFSHOST_CORE_UTIL_NUMBER_HPP
