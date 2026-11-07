#ifndef MPFSHOST_HOST_VMM_MAPS_HPP
#define MPFSHOST_HOST_VMM_MAPS_HPP
#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <optional>
#include <string>
#include <vector>

#include "vmmdll.h"

namespace mpfshost
{
    [[nodiscard]] std::optional<std::vector<MemoryMapRegion>>
    listVmmMemoryMap(VMM_HANDLE handle, const MemoryMapRequest& request, std::string& error);

    [[nodiscard]] std::optional<std::vector<VadExDescriptor>>
    listVmmVadEx(VMM_HANDLE handle, const VadExListRequest& request, LookupStatus& status, std::string& error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_MAPS_HPP
