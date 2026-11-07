#ifndef MPFSHOST_HOST_VMM_KERNEL_OBJECTS_HPP
#define MPFSHOST_HOST_VMM_KERNEL_OBJECTS_HPP

#pragma once

#include "core/server/kernel_object_provider.hpp"

#include <optional>
#include <string>

#include "vmmdll.h"

namespace mpfshost
{
    [[nodiscard]] std::optional<KernelObjectListResult> listVmmKernelObjects(VMM_HANDLE                handle,
                                                                             const KObjectListRequest& request,
                                                                             LookupStatus&             status,
                                                                             std::string&              error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_KERNEL_OBJECTS_HPP
