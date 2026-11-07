#ifndef MPFSHOST_CORE_SERVER_NETWORK_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_NETWORK_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    struct NetworkListResult
    {
        std::vector<NetworkEntryDescriptor> entries;
        bool                                truncated {};
    };

    class NetworkProvider
    {
    public:
        virtual ~NetworkProvider() = default;

        [[nodiscard]] virtual std::optional<NetworkListResult>
        listNetwork(const NetworkListRequest& request, LookupStatus& status, std::string& error) = 0;
    };

    // A non-zero request pid narrows the answer to that process; the text filter then matches the process name or
    // either endpoint.
    [[nodiscard]] bool matchesNetworkFilter(const NetworkListRequest& request, const NetworkEntryDescriptor& entry);
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_NETWORK_PROVIDER_HPP
