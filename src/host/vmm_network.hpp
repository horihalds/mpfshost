#ifndef MPFSHOST_HOST_VMM_NETWORK_HPP
#define MPFSHOST_HOST_VMM_NETWORK_HPP

#pragma once

#include "core/server/network_provider.hpp"

#include <optional>
#include <string>

#include "vmmdll.h"

namespace mpfshost
{
    [[nodiscard]] std::optional<NetworkListResult>
    listVmmNetwork(VMM_HANDLE handle, const NetworkListRequest& request, LookupStatus& status, std::string& error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_NETWORK_HPP
