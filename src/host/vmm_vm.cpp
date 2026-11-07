#include "host/vmm_vm.hpp"

#include "host/vmm_text.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

namespace mpfshost
{
    namespace
    {
        // MemProcFS exposes no VM map entry for the target it is attached to, so the bound target is described by a
        // synthetic entry placed first in every listing.
        constexpr std::string_view kTargetVmName = "physical";

        std::uint64_t queryTargetGpaMax(VMM_HANDLE handle)
        {
            ULONG64 nativeMax = 0;
            if (VMMDLL_ConfigGet(handle, VMMDLL_OPT_CORE_MAX_NATIVE_ADDRESS, &nativeMax) && nativeMax != 0)
            {
                return nativeMax;
            }

            PVMMDLL_MAP_PHYSMEM physMap = nullptr;
            const BOOL          ok      = VMMDLL_Map_GetPhysMem(handle, &physMap);
            const std::unique_ptr<VMMDLL_MAP_PHYSMEM, decltype(&VMMDLL_MemFree)> guard {physMap, &VMMDLL_MemFree};
            if (!ok || physMap == nullptr)
            {
                return 0;
            }

            std::uint64_t gpaMax = 0;
            for (DWORD i = 0; i < physMap->cMap; ++i)
            {
                const auto rangeEnd =
                    static_cast<std::uint64_t>(physMap->pMap[i].pa) + static_cast<std::uint64_t>(physMap->pMap[i].cb);
                gpaMax = std::max(gpaMax, rangeEnd);
            }
            return gpaMax;
        }

        VmDescriptor targetDescriptor(VMM_HANDLE handle)
        {
            VmDescriptor descriptor;
            descriptor.name   = std::string {kTargetVmName};
            descriptor.gpaMax = queryTargetGpaMax(handle);
            descriptor.active = true;

            ULONG64 value = 0;
            if (VMMDLL_ConfigGet(handle, VMMDLL_OPT_CORE_SYSTEM, &value))
            {
                descriptor.systemKind = static_cast<std::uint16_t>(value);
            }
            if (VMMDLL_ConfigGet(handle, VMMDLL_OPT_WIN_VERSION_BUILD, &value))
            {
                descriptor.versionBuild = static_cast<std::uint32_t>(value);
            }
            return descriptor;
        }

        VmDescriptor toDescriptor(const VMMDLL_MAP_VMENTRY& entry)
        {
            VmDescriptor descriptor;
            descriptor.name         = copyMapText(entry.uszName);
            descriptor.gpaMax       = entry.gpaMax;
            descriptor.kind         = static_cast<std::uint16_t>(entry.tp);
            descriptor.systemKind   = static_cast<std::uint16_t>(entry.tpSystem);
            descriptor.partitionId  = entry.dwPartitionID;
            descriptor.versionBuild = entry.dwVersionBuild;
            descriptor.vmMemPid     = entry.dwVmMemPID;
            descriptor.active       = entry.fActive != 0;
            descriptor.readOnly     = entry.fReadOnly != 0;
            descriptor.physicalOnly = entry.fPhysicalOnly != 0;
            return descriptor;
        }
    } // namespace

    std::optional<std::vector<VmDescriptor>> listVmmVms(VMM_HANDLE handle, std::string& error)
    {
        if (handle == nullptr)
        {
            error = "VM handle is closed";
            return std::nullopt;
        }

        std::vector<VmDescriptor> vms;
        vms.push_back(targetDescriptor(handle));

        PVMMDLL_MAP_VM                                                  vmMap = nullptr;
        const BOOL                                                      ok    = VMMDLL_Map_GetVMU(handle, &vmMap);
        const std::unique_ptr<VMMDLL_MAP_VM, decltype(&VMMDLL_MemFree)> guard {vmMap, &VMMDLL_MemFree};
        if (!ok || vmMap == nullptr)
        {
            // VM parsing is disabled by default and only finds child partitions on a hypervisor host, so the target
            // alone is a normal, complete answer.
            return vms;
        }

        vms.reserve(vms.size() + vmMap->cMap);
        for (std::size_t i = 0; i < vmMap->cMap; ++i)
        {
            vms.push_back(toDescriptor(vmMap->pMap[i]));
        }
        return vms;
    }

    std::optional<VmResolution> resolveVmmVm(VMM_HANDLE handle, std::uint32_t vmIndex, std::string& error)
    {
        if (handle == nullptr)
        {
            error = "VM handle is closed";
            return std::nullopt;
        }

        if (vmIndex == 0)
        {
            return VmResolution {nullptr, queryTargetGpaMax(handle)};
        }

        PVMMDLL_MAP_VM                                                  vmMap = nullptr;
        const BOOL                                                      ok    = VMMDLL_Map_GetVMU(handle, &vmMap);
        const std::unique_ptr<VMMDLL_MAP_VM, decltype(&VMMDLL_MemFree)> guard {vmMap, &VMMDLL_MemFree};
        if (!ok || vmMap == nullptr)
        {
            error = "VMMDLL_Map_GetVMU failed";
            return std::nullopt;
        }
        if (vmIndex - 1 >= vmMap->cMap)
        {
            error = "VM partition " + std::to_string(vmIndex) + " does not exist";
            return std::nullopt;
        }

        const auto& entry = vmMap->pMap[vmIndex - 1];
        return VmResolution {entry.hVM, entry.gpaMax};
    }
} // namespace mpfshost
