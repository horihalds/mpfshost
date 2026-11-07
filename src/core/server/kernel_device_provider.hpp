#ifndef MPFSHOST_CORE_SERVER_KERNEL_DEVICE_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_KERNEL_DEVICE_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    struct KernelDeviceListResult
    {
        std::vector<KernelDeviceDescriptor> devices;
        bool                                truncated {};
    };

    class KernelDeviceProvider
    {
    public:
        virtual ~KernelDeviceProvider() = default;

        [[nodiscard]] virtual std::optional<KernelDeviceListResult>
        listKernelDevices(const KDeviceListRequest& request, LookupStatus& status, std::string& error) = 0;
    };

    [[nodiscard]] bool matchesKernelDeviceFilter(const KDeviceListRequest&     request,
                                                 const KernelDeviceDescriptor& device);
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_KERNEL_DEVICE_PROVIDER_HPP
