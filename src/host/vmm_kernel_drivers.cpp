#include "host/vmm_kernel_drivers.hpp"

#include "core/ipc/frame.hpp"
#include "host/vmm_text.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

namespace mpfshost
{
    std::optional<KernelDriverListResult>
    listVmmKernelDrivers(VMM_HANDLE handle, const KDriverListRequest& request, LookupStatus& status, std::string& error)
    {
        (void)status;
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        // Like the other whole-machine maps there is no pid to probe, so a failing call cannot be told apart from an
        // empty one; a target whose kernel is not resolved reads as Internal rather than as no drivers.
        PVMMDLL_MAP_KDRIVER                                                  map = nullptr;
        const BOOL                                                           ok  = VMMDLL_Map_GetKDriverU(handle, &map);
        const std::unique_ptr<VMMDLL_MAP_KDRIVER, decltype(&VMMDLL_MemFree)> guard {map, &VMMDLL_MemFree};
        if (!ok || map == nullptr)
        {
            error = "VMMDLL_Map_GetKDriverU failed";
            return std::nullopt;
        }

        KernelDriverListResult result;
        result.drivers.reserve(std::min<std::size_t>(map->cMap, kMaxKernelDriverEntries));
        for (DWORD i = 0; i < map->cMap; ++i)
        {
            const auto&            entry = map->pMap[i];
            KernelDriverDescriptor driver;
            driver.va             = entry.va;
            driver.vaDriverStart  = entry.vaDriverStart;
            driver.cbDriverSize   = entry.cbDriverSize;
            driver.vaDeviceObject = entry.vaDeviceObject;
            driver.name           = copyMapText(entry.uszName);
            driver.path           = copyMapText(entry.uszPath);
            driver.serviceKeyName = copyMapText(entry.uszServiceKeyName);

            // MajorFunction is a fixed 28-slot table; trim it at its last populated slot so an untouched slot costs
            // nothing while index i still means IRP_MJ_* slot i.
            std::size_t populated = 0;
            for (std::size_t slot = 0; slot < std::size(entry.MajorFunction); ++slot)
            {
                if (entry.MajorFunction[slot] != 0)
                {
                    populated = slot + 1;
                }
            }
            driver.majorFunctions.assign(entry.MajorFunction, entry.MajorFunction + populated);

            if (!matchesKernelDriverFilter(request, driver))
            {
                continue;
            }
            if (result.drivers.size() == kMaxKernelDriverEntries)
            {
                result.truncated = true;
                break;
            }
            result.drivers.push_back(std::move(driver));
        }
        return result;
    }
} // namespace mpfshost
