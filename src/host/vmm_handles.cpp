#include "host/vmm_handles.hpp"

#include "core/ipc/frame.hpp"
#include "host/vmm_process_probe.hpp"
#include "host/vmm_text.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>

namespace mpfshost
{
    std::optional<HandleListResult>
    listVmmHandles(VMM_HANDLE handle, const HandleListRequest& request, LookupStatus& status, std::string& error)
    {
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        PVMMDLL_MAP_HANDLE map = nullptr;
        const BOOL         ok  = VMMDLL_Map_GetHandleU(handle, request.pid, &map);
        const std::unique_ptr<VMMDLL_MAP_HANDLE, decltype(&VMMDLL_MemFree)> guard {map, &VMMDLL_MemFree};
        if (!ok || map == nullptr)
        {
            if (!processPresent(handle, request.pid))
            {
                status = LookupStatus::NotFound;
                error  = "process " + std::to_string(request.pid) + " is not present";
                return std::nullopt;
            }
            error = "VMMDLL_Map_GetHandleU failed";
            return std::nullopt;
        }

        HandleListResult result;
        result.handles.reserve(std::min<std::size_t>(map->cMap, kMaxHandleEntries));
        for (DWORD i = 0; i < map->cMap; ++i)
        {
            const auto&      entry = map->pMap[i];
            HandleDescriptor descriptor;
            descriptor.vaObject           = entry.vaObject;
            descriptor.handle             = entry.dwHandle;
            descriptor.grantedAccess      = entry.dwGrantedAccess;
            descriptor.typeIndex          = entry.iType;
            descriptor.handleCount        = entry.qwHandleCount;
            descriptor.pointerCount       = entry.qwPointerCount;
            descriptor.objectCreateInfo   = entry.vaObjectCreateInfo;
            descriptor.securityDescriptor = entry.vaSecurityDescriptor;
            descriptor.poolTag            = entry.dwPoolTag;
            descriptor.text               = copyMapText(entry.uszText);
            descriptor.type               = copyMapText(entry.uszType);

            if (!matchesHandleFilter(request, descriptor))
            {
                continue;
            }
            if (result.handles.size() == kMaxHandleEntries)
            {
                result.truncated = true;
                break;
            }
            result.handles.push_back(std::move(descriptor));
        }
        return result;
    }
} // namespace mpfshost
