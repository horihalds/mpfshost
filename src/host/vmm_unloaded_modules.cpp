#include "host/vmm_unloaded_modules.hpp"

#include "core/ipc/frame.hpp"
#include "host/vmm_process_probe.hpp"
#include "host/vmm_text.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace mpfshost
{
    std::optional<UnloadedModuleListResult> listVmmUnloadedModules(VMM_HANDLE                       handle,
                                                                   const UnloadedModuleListRequest& request,
                                                                   LookupStatus&                    status,
                                                                   std::string&                     error)
    {
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        PVMMDLL_MAP_UNLOADEDMODULE map = nullptr;
        const BOOL                 ok  = VMMDLL_Map_GetUnloadedModuleU(handle, request.pid, &map);
        const std::unique_ptr<VMMDLL_MAP_UNLOADEDMODULE, decltype(&VMMDLL_MemFree)> guard {map, &VMMDLL_MemFree};
        if (!ok || map == nullptr)
        {
            if (!processPresent(handle, request.pid))
            {
                status = LookupStatus::NotFound;
                error  = "process " + std::to_string(request.pid) + " is not present";
                return std::nullopt;
            }
            error = "VMMDLL_Map_GetUnloadedModuleU failed";
            return std::nullopt;
        }

        UnloadedModuleListResult result;
        result.modules.reserve(std::min<std::size_t>(map->cMap, kMaxUnloadedModuleEntries));
        for (DWORD i = 0; i < map->cMap; ++i)
        {
            const auto&              entry = map->pMap[i];
            UnloadedModuleDescriptor module;
            module.vaBase        = entry.vaBase;
            module.imageSize     = entry.cbImageSize;
            module.isWow64       = entry.fWoW64 != 0;
            module.checksum      = entry.dwCheckSum;
            module.timeDateStamp = entry.dwTimeDateStamp;
            module.unloadTime    = entry.ftUnload;
            module.text          = copyMapText(entry.uszText);

            if (!matchesUnloadedModuleFilter(request, module))
            {
                continue;
            }
            if (result.modules.size() == kMaxUnloadedModuleEntries)
            {
                result.truncated = true;
                break;
            }
            result.modules.push_back(std::move(module));
        }
        return result;
    }
} // namespace mpfshost
