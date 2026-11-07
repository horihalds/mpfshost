#ifndef MPFSHOST_HOST_VMM_MODULES_HPP
#define MPFSHOST_HOST_VMM_MODULES_HPP
#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <optional>
#include <string>
#include <vector>

#include "vmmdll.h"

namespace mpfshost
{
    [[nodiscard]] std::optional<std::vector<ModuleDescriptor>>
    listVmmModules(VMM_HANDLE handle, const ModuleListRequest& request, std::string& error);

    [[nodiscard]] std::optional<SymbolLookupResponse>
    lookupVmmSymbol(VMM_HANDLE handle, const SymbolLookupRequest& request, LookupStatus& status, std::string& error);

    [[nodiscard]] std::optional<ModuleLayoutResponse> queryVmmModuleLayout(VMM_HANDLE                 handle,
                                                                           const ModuleLayoutRequest& request,
                                                                           LookupStatus&              status,
                                                                           std::string&               error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_MODULES_HPP
