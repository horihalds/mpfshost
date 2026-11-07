#ifndef MPFSHOST_CORE_SERVER_USER_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_USER_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    struct UserListResult
    {
        std::vector<UserDescriptor> users;
        bool                        truncated {};
    };

    class UserProvider
    {
    public:
        virtual ~UserProvider() = default;

        [[nodiscard]] virtual std::optional<UserListResult>
        listUsers(const UserListRequest& request, LookupStatus& status, std::string& error) = 0;
    };

    [[nodiscard]] bool matchesUserFilter(const UserListRequest& request, const UserDescriptor& user);
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_USER_PROVIDER_HPP
