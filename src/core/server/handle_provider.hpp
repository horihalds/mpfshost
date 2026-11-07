#ifndef MPFSHOST_CORE_SERVER_HANDLE_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_HANDLE_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    struct HandleListResult
    {
        std::vector<HandleDescriptor> handles;
        bool                          truncated {};
    };

    class HandleProvider
    {
    public:
        virtual ~HandleProvider() = default;

        [[nodiscard]] virtual std::optional<HandleListResult>
        listHandles(const HandleListRequest& request, LookupStatus& status, std::string& error) = 0;
    };

    [[nodiscard]] bool matchesHandleFilter(const HandleListRequest& request, const HandleDescriptor& handle);
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_HANDLE_PROVIDER_HPP
