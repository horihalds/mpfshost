#ifndef MPFSHOST_HOST_VMM_POOL_HPP
#define MPFSHOST_HOST_VMM_POOL_HPP

#pragma once

#include "core/server/pool_provider.hpp"

#include <optional>
#include <string>

#include "vmmdll.h"

namespace mpfshost
{
    [[nodiscard]] std::optional<PoolListResult>
    listVmmPool(VMM_HANDLE handle, const PoolListRequest& request, LookupStatus& status, std::string& error);

    [[nodiscard]] std::optional<PoolTagListResult>
    listVmmPoolTags(VMM_HANDLE handle, const PoolTagListRequest& request, LookupStatus& status, std::string& error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_POOL_HPP
