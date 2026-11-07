#include "core/server/network_provider.hpp"

#include "core/util/text.hpp"

namespace mpfshost
{
    bool matchesNetworkFilter(const NetworkListRequest& request, const NetworkEntryDescriptor& entry)
    {
        if (request.pid != 0 && entry.pid != request.pid)
        {
            return false;
        }
        return containsCaseInsensitive(entry.process, request.filter)
            || containsCaseInsensitive(entry.local.address, request.filter)
            || containsCaseInsensitive(entry.remote.address, request.filter);
    }
} // namespace mpfshost
