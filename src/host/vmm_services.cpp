#include "host/vmm_services.hpp"

#include "core/ipc/frame.hpp"
#include "host/vmm_text.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace mpfshost
{
    std::optional<ServiceListResult>
    listVmmServices(VMM_HANDLE handle, const ServiceListRequest& request, LookupStatus& status, std::string& error)
    {
        (void)status;
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        // The service map is a whole-machine map with no pid to probe, so a failing call cannot be told apart from
        // an empty one; a target whose system maps stay unparsed therefore reads as Internal, not as no services.
        PVMMDLL_MAP_SERVICE                                                  map = nullptr;
        const BOOL                                                           ok = VMMDLL_Map_GetServicesU(handle, &map);
        const std::unique_ptr<VMMDLL_MAP_SERVICE, decltype(&VMMDLL_MemFree)> guard {map, &VMMDLL_MemFree};
        if (!ok || map == nullptr)
        {
            error = "VMMDLL_Map_GetServicesU failed";
            return std::nullopt;
        }

        ServiceListResult result;
        result.services.reserve(std::min<std::size_t>(map->cMap, kMaxServiceEntries));
        for (DWORD i = 0; i < map->cMap; ++i)
        {
            const auto&       entry = map->pMap[i];
            ServiceDescriptor service;
            service.objectAddress           = entry.vaObj;
            service.ordinal                 = entry.dwOrdinal;
            service.startType               = entry.dwStartType;
            service.serviceType             = entry.ServiceStatus.dwServiceType;
            service.state                   = entry.ServiceStatus.dwCurrentState;
            service.controlsAccepted        = entry.ServiceStatus.dwControlsAccepted;
            service.win32ExitCode           = entry.ServiceStatus.dwWin32ExitCode;
            service.serviceSpecificExitCode = entry.ServiceStatus.dwServiceSpecificExitCode;
            service.checkPoint              = entry.ServiceStatus.dwCheckPoint;
            service.waitHint                = entry.ServiceStatus.dwWaitHint;
            service.pid                     = entry.dwPID;
            service.name                    = copyMapText(entry.uszServiceName);
            service.displayName             = copyMapText(entry.uszDisplayName);
            service.imagePath               = copyMapText(entry.uszImagePath);
            service.path                    = copyMapText(entry.uszPath);
            service.userType                = copyMapText(entry.uszUserTp);
            service.account                 = copyMapText(entry.uszUserAcct);

            if (!matchesServiceFilter(request, service))
            {
                continue;
            }
            if (result.services.size() == kMaxServiceEntries)
            {
                result.truncated = true;
                break;
            }
            result.services.push_back(std::move(service));
        }
        return result;
    }
} // namespace mpfshost
