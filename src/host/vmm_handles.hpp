#ifndef MPFSHOST_HOST_VMM_HANDLES_HPP
#define MPFSHOST_HOST_VMM_HANDLES_HPP
#pragma once

#include "core/server/handle_provider.hpp"

#include <optional>
#include <string>

#include "vmmdll.h"

namespace mpfshost
{
    [[nodiscard]] std::optional<HandleListResult>
    listVmmHandles(VMM_HANDLE handle, const HandleListRequest& request, LookupStatus& status, std::string& error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_HANDLES_HPP
