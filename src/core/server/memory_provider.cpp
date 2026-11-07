#include "core/server/memory_provider.hpp"

#include "core/ipc/frame.hpp"

#include <limits>

namespace mpfshost
{
    bool isValidMemoryRange(std::uint64_t address, std::uint32_t size)
    {
        if (size == 0 || size > kMaxReadSize)
        {
            return false;
        }
        return size <= std::numeric_limits<std::uint64_t>::max() - address;
    }
} // namespace mpfshost
