#ifndef MPFSHOST_HOST_VMM_USERS_HPP
#define MPFSHOST_HOST_VMM_USERS_HPP

#pragma once

#include "core/server/user_provider.hpp"

#include <optional>
#include <string>

#include "vmmdll.h"

namespace mpfshost
{
    [[nodiscard]] std::optional<UserListResult>
    listVmmUsers(VMM_HANDLE handle, const UserListRequest& request, LookupStatus& status, std::string& error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_USERS_HPP
