#include "host/vmm_pte.hpp"

#include "core/ipc/frame.hpp"
#include "core/server/request_deadline.hpp"
#include "host/vmm_process_probe.hpp"
#include "host/vmm_text.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>

namespace mpfshost
{
    std::optional<PteListResult>
    listVmmPte(VMM_HANDLE handle, const PteListRequest& request, LookupStatus& status, std::string& error)
    {
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        if (requestExpired())
        {
            error = "request deadline exceeded";
            return std::nullopt;
        }

        PVMMDLL_MAP_PTE map = nullptr;
        const BOOL      ok  = VMMDLL_Map_GetPteU(handle, request.pid, request.identifyModules != 0 ? 1 : 0, &map);
        const std::unique_ptr<VMMDLL_MAP_PTE, decltype(&VMMDLL_MemFree)> guard {map, &VMMDLL_MemFree};
        if (!ok || map == nullptr)
        {
            if (!processPresent(handle, request.pid))
            {
                status = LookupStatus::NotFound;
                error  = "process " + std::to_string(request.pid) + " is not present";
                return std::nullopt;
            }
            error = "VMMDLL_Map_GetPteU failed";
            return std::nullopt;
        }
        if (requestExpired())
        {
            error = "request deadline exceeded";
            return std::nullopt;
        }

        PteListResult result;
        result.mapEntries = map->cMap;
        result.entries.reserve(std::min<std::size_t>(map->cMap, kMaxPteEntries));
        for (DWORD i = 0; i < map->cMap; ++i)
        {
            const auto&   entry = map->pMap[i];
            PteDescriptor region;
            region.vaBase        = entry.vaBase;
            region.pages         = entry.cPages;
            region.pageFlags     = entry.fPage;
            region.softwarePages = entry.cSoftware;
            region.wow64         = entry.fWoW64 != 0 ? 1 : 0;
            // uszText points into the map and is only filled when module identification was requested.
            if (request.identifyModules != 0)
            {
                region.text = copyMapText(entry.uszText);
            }

            if (!matchesPteFilter(request, region))
            {
                continue;
            }
            if (result.entries.size() == kMaxPteEntries)
            {
                result.truncated = true;
                break;
            }
            result.entries.push_back(std::move(region));
        }
        return result;
    }
} // namespace mpfshost
