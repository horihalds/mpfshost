#include "host/device_string.hpp"

namespace mpfshost
{
    std::string qemuDeviceString(const VmTarget& target)
    {
        return "qemu://hugepage-pid=" + std::to_string(target.qemuPid) + ",qmp=" + target.qmpSocket.string();
    }
} // namespace mpfshost
