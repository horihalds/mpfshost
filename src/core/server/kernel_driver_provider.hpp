#ifndef MPFSHOST_CORE_SERVER_KERNEL_DRIVER_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_KERNEL_DRIVER_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    struct KernelDriverListResult
    {
        std::vector<KernelDriverDescriptor> drivers;
        bool                                truncated {};
    };

    class KernelDriverProvider
    {
    public:
        virtual ~KernelDriverProvider() = default;

        [[nodiscard]] virtual std::optional<KernelDriverListResult>
        listKernelDrivers(const KDriverListRequest& request, LookupStatus& status, std::string& error) = 0;
    };

    [[nodiscard]] bool matchesKernelDriverFilter(const KDriverListRequest&     request,
                                                 const KernelDriverDescriptor& driver);
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_KERNEL_DRIVER_PROVIDER_HPP
