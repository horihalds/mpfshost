#include "core/server/vm_provider.hpp"

namespace mpfshost
{
    const VmDescriptor* findVm(const std::vector<VmDescriptor>& vms, std::uint32_t vmIndex)
    {
        if (vmIndex >= vms.size())
        {
            return nullptr;
        }
        return &vms[vmIndex];
    }
} // namespace mpfshost
