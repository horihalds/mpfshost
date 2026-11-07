#include "core/server/pte_provider.hpp"

#include "core/ipc/frame.hpp"
#include "core/util/text.hpp"

#include <limits>

namespace mpfshost
{
    bool matchesPteFilter(const PteListRequest& request, const PteDescriptor& entry)
    {
        if (request.minAddress != 0 || request.maxAddress != 0)
        {
            // The map describes a whole region, so the window matches when the region overlaps it at all rather
            // than only when its base falls inside.
            constexpr std::uint64_t kMax      = std::numeric_limits<std::uint64_t>::max();
            const bool              clipped   = entry.pages > (kMax - entry.vaBase) / kPageSize;
            const std::uint64_t     regionEnd = clipped ? kMax : entry.vaBase + entry.pages * kPageSize;
            if (request.minAddress != 0 && !clipped && regionEnd <= request.minAddress)
            {
                return false;
            }
            if (request.maxAddress != 0 && entry.vaBase >= request.maxAddress)
            {
                return false;
            }
        }
        return containsCaseInsensitive(entry.text, request.filter);
    }
} // namespace mpfshost
