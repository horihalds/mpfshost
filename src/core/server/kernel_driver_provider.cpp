#include "core/server/kernel_driver_provider.hpp"

#include "core/util/text.hpp"

namespace mpfshost
{
    bool matchesKernelDriverFilter(const KDriverListRequest& request, const KernelDriverDescriptor& driver)
    {
        return containsCaseInsensitive(driver.name, request.filter)
            || containsCaseInsensitive(driver.path, request.filter)
            || containsCaseInsensitive(driver.serviceKeyName, request.filter);
    }
} // namespace mpfshost
