#ifndef MPFSHOST_HOST_VMM_UNLOADED_MODULES_HPP
#define MPFSHOST_HOST_VMM_UNLOADED_MODULES_HPP
#pragma once

#include "core/server/unloaded_module_provider.hpp"

#include <optional>
#include <string>

#include "vmmdll.h"

namespace mpfshost
{
    [[nodiscard]] std::optional<UnloadedModuleListResult> listVmmUnloadedModules(
        VMM_HANDLE handle, const UnloadedModuleListRequest& request, LookupStatus& status, std::string& error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_UNLOADED_MODULES_HPP
