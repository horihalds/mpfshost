#include "core/server/process_provider.hpp"

#include "core/util/text.hpp"

namespace mpfshost
{
    bool matchesProcessFilter(const ProcessFilter& filter, const ProcessDescriptor& process)
    {
        if (filter.pid != 0 && filter.pid != process.pid)
        {
            return false;
        }
        return containsCaseInsensitive(process.name, filter.nameFilter);
    }
} // namespace mpfshost
