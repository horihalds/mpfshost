#ifndef MPFSHOST_CORE_UTIL_DISPLAY_HPP
#define MPFSHOST_CORE_UTIL_DISPLAY_HPP

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mpfshost
{
    // MemProcFS reports an export with no names table as this offset instead of omitting the column.
    inline constexpr std::uint32_t kAbsentArrayOffset = 0xFFFFFFFFu;

    [[nodiscard]] std::string exportNamesArrayText(std::uint32_t offset);

    // MemProcFS sometimes fills a name that has no value with a run of dashes; the CLI shows a single dash.
    [[nodiscard]] std::string kernelDeviceTypeText(std::string_view name);
} // namespace mpfshost

#endif // MPFSHOST_CORE_UTIL_DISPLAY_HPP
