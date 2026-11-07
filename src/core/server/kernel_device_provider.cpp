#include "core/server/kernel_device_provider.hpp"

#include "core/util/text.hpp"

namespace mpfshost
{
    bool matchesKernelDeviceFilter(const KDeviceListRequest& request, const KernelDeviceDescriptor& device)
    {
        return containsCaseInsensitive(device.deviceTypeName, request.filter)
            || containsCaseInsensitive(device.volumeInfo, request.filter);
    }
} // namespace mpfshost
