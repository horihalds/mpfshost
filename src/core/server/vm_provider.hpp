#ifndef MPFSHOST_CORE_SERVER_VM_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_VM_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    class VmProvider
    {
    public:
        virtual ~VmProvider() = default;

        [[nodiscard]] virtual std::optional<std::vector<VmDescriptor>> listVms(std::string& error) = 0;
    };

    // VM partitions are addressed by their position in the list, so every request re-checks the index.
    [[nodiscard]] const VmDescriptor* findVm(const std::vector<VmDescriptor>& vms, std::uint32_t vmIndex);
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_VM_PROVIDER_HPP
