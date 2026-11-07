#include "core/util/page_range.hpp"

#include <algorithm>
#include <limits>

namespace mpfshost
{
    namespace
    {
        constexpr std::uint64_t kPageOffsetMask = kPageSize - 1;

        bool spanPages(const PageRange& range, std::uint64_t& start, std::uint64_t& pages)
        {
            if (range.size == 0)
            {
                return false;
            }
            if (range.size > std::numeric_limits<std::uint64_t>::max() - range.address)
            {
                return false;
            }

            // An unaligned address belongs to the page it fell into, so the expansion always starts at the
            // containing page.
            start           = range.address & ~kPageOffsetMask;
            const auto end  = range.address + range.size;
            const auto span = end - start;
            pages           = span / kPageSize + ((span % kPageSize) != 0 ? 1 : 0);
            return pages != 0;
        }
    } // namespace

    std::optional<std::vector<std::uint64_t>> expandPageAddresses(std::span<const PageRange> ranges,
                                                                  std::size_t                maxPages)
    {
        std::size_t total = 0;
        for (const auto& range : ranges)
        {
            std::uint64_t start {};
            std::uint64_t pages {};
            if (!spanPages(range, start, pages))
            {
                return std::nullopt;
            }
            if (pages > static_cast<std::uint64_t>(maxPages) || total > maxPages - static_cast<std::size_t>(pages))
            {
                return std::nullopt;
            }
            total += static_cast<std::size_t>(pages);
        }
        if (total == 0)
        {
            return std::nullopt;
        }

        std::vector<std::uint64_t> addresses;
        addresses.reserve(total);
        for (const auto& range : ranges)
        {
            std::uint64_t start {};
            std::uint64_t pages {};
            static_cast<void>(spanPages(range, start, pages));
            for (std::uint64_t index = 0; index < pages; ++index)
            {
                addresses.push_back(start + index * kPageSize);
            }
        }

        std::sort(addresses.begin(), addresses.end());
        addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
        return addresses;
    }
} // namespace mpfshost
