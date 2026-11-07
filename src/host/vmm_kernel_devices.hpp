#ifndef MPFSHOST_HOST_VMM_KERNEL_DEVICES_HPP
#define MPFSHOST_HOST_VMM_KERNEL_DEVICES_HPP

#pragma once

#include "core/server/kernel_device_provider.hpp"

#include <optional>
#include <string>

#include "vmmdll.h"

namespace mpfshost
{
    [[nodiscard]] std::optional<KernelDeviceListResult> listVmmKernelDevices(VMM_HANDLE                handle,
                                                                             const KDeviceListRequest& request,
                                                                             LookupStatus&             status,
                                                                             std::string&              error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_KERNEL_DEVICES_HPP
