#include "host/vmm_kernel_objects.hpp"

#include "core/ipc/frame.hpp"
#include "host/vmm_text.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace mpfshost
{
    std::optional<KernelObjectListResult>
    listVmmKernelObjects(VMM_HANDLE handle, const KObjectListRequest& request, LookupStatus& status, std::string& error)
    {
        (void)status;
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        // Whole-machine map: no pid to probe, so a failing call is Internal rather than an empty answer.
        PVMMDLL_MAP_KOBJECT                                                  map = nullptr;
        const BOOL                                                           ok  = VMMDLL_Map_GetKObjectU(handle, &map);
        const std::unique_ptr<VMMDLL_MAP_KOBJECT, decltype(&VMMDLL_MemFree)> guard {map, &VMMDLL_MemFree};
        if (!ok || map == nullptr)
        {
            error = "VMMDLL_Map_GetKObjectU failed";
            return std::nullopt;
        }

        KernelObjectListResult result;
        result.objects.reserve(std::min<std::size_t>(map->cMap, kMaxKernelObjectEntries));
        for (DWORD i = 0; i < map->cMap; ++i)
        {
            const auto&            entry = map->pMap[i];
            KernelObjectDescriptor object;
            object.va         = entry.va;
            object.vaParent   = entry.vaParent;
            object.childCount = entry.cvaChild;
            object.name       = copyMapText(entry.uszName);
            object.type       = copyMapText(entry.uszType);

            // pvaChild is unbounded and only valid while the map lives, so copy at most the per-object cap and keep
            // the true total so a client can tell a wide object from a short one.
            if (entry.pvaChild != nullptr && entry.cvaChild != 0)
            {
                const std::size_t copyCount =
                    std::min<std::size_t>(entry.cvaChild, static_cast<std::size_t>(kMaxKernelObjectChildren));
                object.children.assign(entry.pvaChild, entry.pvaChild + copyCount);
            }
            object.childrenTruncated = object.children.size() < entry.cvaChild;

            if (!matchesKernelObjectFilter(request, object))
            {
                continue;
            }
            if (result.objects.size() == kMaxKernelObjectEntries)
            {
                result.truncated = true;
                break;
            }
            result.objects.push_back(std::move(object));
        }
        return result;
    }
} // namespace mpfshost
