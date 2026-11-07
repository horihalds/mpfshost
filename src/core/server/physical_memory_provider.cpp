#include "core/server/physical_memory_provider.hpp"

#include "core/ipc/frame.hpp"

namespace mpfshost
{
    bool isWithinPhysicalRange(std::uint64_t gpa, std::uint64_t size, std::uint64_t gpaMax)
    {
        return size <= gpaMax && gpa <= gpaMax - size;
    }

    bool isValidPhysicalScatterEntry(const ScatterEntry& entry)
    {
        if (entry.size < kPhysicalScatterMinBytes || entry.size > kPhysicalScatterPageSize)
        {
            return false;
        }
        if ((entry.address % sizeof(std::uint32_t)) != 0)
        {
            return false;
        }
        if ((entry.address % kPhysicalScatterPageSize) + entry.size > kPhysicalScatterPageSize)
        {
            return false;
        }

        if (entry.kind == kScatterEntryRead)
        {
            return entry.data.empty();
        }
        if (entry.kind == kScatterEntryWrite)
        {
            return entry.data.size() == entry.size;
        }
        return false;
    }
} // namespace mpfshost
