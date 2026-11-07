#ifndef MPFSHOST_CORE_SERVER_KERNEL_OBJECT_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_KERNEL_OBJECT_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    struct KernelObjectListResult
    {
        std::vector<KernelObjectDescriptor> objects;
        bool                                truncated {};
    };

    class KernelObjectProvider
    {
    public:
        virtual ~KernelObjectProvider() = default;

        [[nodiscard]] virtual std::optional<KernelObjectListResult>
        listKernelObjects(const KObjectListRequest& request, LookupStatus& status, std::string& error) = 0;
    };

    [[nodiscard]] bool matchesKernelObjectFilter(const KObjectListRequest&     request,
                                                 const KernelObjectDescriptor& object);
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_KERNEL_OBJECT_PROVIDER_HPP
