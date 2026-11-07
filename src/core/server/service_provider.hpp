#ifndef MPFSHOST_CORE_SERVER_SERVICE_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_SERVICE_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    struct ServiceListResult
    {
        std::vector<ServiceDescriptor> services;
        bool                           truncated {};
    };

    class ServiceProvider
    {
    public:
        virtual ~ServiceProvider() = default;

        [[nodiscard]] virtual std::optional<ServiceListResult>
        listServices(const ServiceListRequest& request, LookupStatus& status, std::string& error) = 0;
    };

    [[nodiscard]] bool matchesServiceFilter(const ServiceListRequest& request, const ServiceDescriptor& service);
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_SERVICE_PROVIDER_HPP
