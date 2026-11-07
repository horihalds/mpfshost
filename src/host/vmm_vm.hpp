#ifndef MPFSHOST_HOST_VMM_VM_HPP
#define MPFSHOST_HOST_VMM_VM_HPP

#pragma once

#include "core/ipc/message.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vmmdll.h"

namespace mpfshost
{
    struct VmResolution
    {
        // A null handle addresses the bound target's own physical memory, which MemProcFS serves with dwPID = -1.
        VMMVM_HANDLE  vm {};
        std::uint64_t gpaMax {};
    };

    // Index 0 is always the bound target itself; the child virtual machines found by VMMDLL_Map_GetVMU, which only
    // exist when MemProcFS VM parsing is enabled on a hypervisor host, follow it.
    [[nodiscard]] std::optional<std::vector<VmDescriptor>> listVmmVms(VMM_HANDLE handle, std::string& error);

    // The VM handle is only valid while the VMM_HANDLE lives, so nothing is cached between requests.
    [[nodiscard]] std::optional<VmResolution>
    resolveVmmVm(VMM_HANDLE handle, std::uint32_t vmIndex, std::string& error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_VM_HPP
