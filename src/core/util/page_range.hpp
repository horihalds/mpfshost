#ifndef MPFSHOST_CORE_UTIL_PAGE_RANGE_HPP
#define MPFSHOST_CORE_UTIL_PAGE_RANGE_HPP

#pragma once

#include "core/ipc/message.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace mpfshost
{
    // Page-aligned addresses, ascending and de-duplicated. Nothing when a range is empty or inverted, or when
    // the expansion would exceed `maxPages`; the cap is checked while expanding so no oversized allocation
    // happens first.
    [[nodiscard]] std::optional<std::vector<std::uint64_t>> expandPageAddresses(std::span<const PageRange> ranges,
                                                                                std::size_t                maxPages);
} // namespace mpfshost

#endif // MPFSHOST_CORE_UTIL_PAGE_RANGE_HPP
