#include "host/vmm_kernel_devices.hpp"

#include "core/ipc/frame.hpp"
#include "host/vmm_text.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace mpfshost
{
    std::optional<KernelDeviceListResult>
    listVmmKernelDevices(VMM_HANDLE handle, const KDeviceListRequest& request, LookupStatus& status, std::string& error)
    {
        (void)status;
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        // Whole-machine map: no pid to probe, so a failing call is Internal rather than an empty answer.
        PVMMDLL_MAP_KDEVICE                                                  map = nullptr;
        const BOOL                                                           ok  = VMMDLL_Map_GetKDeviceU(handle, &map);
        const std::unique_ptr<VMMDLL_MAP_KDEVICE, decltype(&VMMDLL_MemFree)> guard {map, &VMMDLL_MemFree};
        if (!ok || map == nullptr)
        {
            error = "VMMDLL_Map_GetKDeviceU failed";
            return std::nullopt;
        }

        KernelDeviceListResult result;
        result.devices.reserve(std::min<std::size_t>(map->cMap, kMaxKernelDeviceEntries));
        for (DWORD i = 0; i < map->cMap; ++i)
        {
            const auto&            entry = map->pMap[i];
            KernelDeviceDescriptor device;
            device.va                 = entry.va;
            device.depth              = entry.iDepth;
            device.deviceType         = entry.dwDeviceType;
            device.vaDriverObject     = entry.vaDriverObject;
            device.vaAttachedDevice   = entry.vaAttachedDevice;
            device.vaFileSystemDevice = entry.vaFileSystemDevice;
            device.deviceTypeName     = copyMapText(entry.uszDeviceType);
            device.volumeInfo         = copyMapText(entry.uszVolumeInfo);

            if (!matchesKernelDeviceFilter(request, device))
            {
                continue;
            }
            if (result.devices.size() == kMaxKernelDeviceEntries)
            {
                result.truncated = true;
                break;
            }
            result.devices.push_back(std::move(device));
        }
        return result;
    }
} // namespace mpfshost
