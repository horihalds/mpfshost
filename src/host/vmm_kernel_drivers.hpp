#ifndef MPFSHOST_HOST_VMM_KERNEL_DRIVERS_HPP
#define MPFSHOST_HOST_VMM_KERNEL_DRIVERS_HPP

#pragma once

#include "core/server/kernel_driver_provider.hpp"

#include <optional>
#include <string>

#include "vmmdll.h"

namespace mpfshost
{
    [[nodiscard]] std::optional<KernelDriverListResult> listVmmKernelDrivers(VMM_HANDLE                handle,
                                                                             const KDriverListRequest& request,
                                                                             LookupStatus&             status,
                                                                             std::string&              error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_KERNEL_DRIVERS_HPP
