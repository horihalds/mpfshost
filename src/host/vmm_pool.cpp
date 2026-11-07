#include "host/vmm_pool.hpp"

#include "core/ipc/frame.hpp"
#include "core/server/request_deadline.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>

namespace mpfshost
{
    std::optional<PoolListResult>
    listVmmPool(VMM_HANDLE handle, const PoolListRequest& request, LookupStatus& status, std::string& error)
    {
        (void)status;
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

        // Whole-machine map: the build happens inside MemProcFS and cannot be interrupted, so the budget is only
        // enforced before the call and around the copy loop below.
        const DWORD      flags = request.big != 0 ? VMMDLL_POOLMAP_FLAG_BIG : VMMDLL_POOLMAP_FLAG_ALL;
        PVMMDLL_MAP_POOL map   = nullptr;
        const BOOL       ok    = VMMDLL_Map_GetPool(handle, &map, flags);
        const std::unique_ptr<VMMDLL_MAP_POOL, decltype(&VMMDLL_MemFree)> guard {map, &VMMDLL_MemFree};
        if (!ok || map == nullptr)
        {
            error = "VMMDLL_Map_GetPool failed";
            return std::nullopt;
        }

        PoolListResult result;
        result.mapEntries = map->cMap;
        result.mapBytes   = map->cbTotal;
        result.entries.reserve(std::min<std::size_t>(map->cMap, kMaxPoolEntries));
        for (DWORD i = 0; i < map->cMap; ++i)
        {
            const auto&    entry = map->pMap[i];
            PoolDescriptor allocation;
            allocation.va         = entry.va;
            allocation.bytes      = entry.cb;
            allocation.tag        = entry.dwTag;
            allocation.alloc      = entry.fAlloc != 0 ? 1 : 0;
            allocation.poolType   = entry.tpPool;
            allocation.subsegment = entry.tpSS;

            if (result.entries.size() == kMaxPoolEntries)
            {
                result.truncated = true;
                break;
            }
            if ((i & 0xFFF) == 0 && requestExpired())
            {
                error = "request deadline exceeded";
                return std::nullopt;
            }
            if (!matchesPoolFilter(request, allocation))
            {
                continue;
            }
            result.entries.push_back(allocation);
        }
        return result;
    }

    std::optional<PoolTagListResult>
    listVmmPoolTags(VMM_HANDLE handle, const PoolTagListRequest& request, LookupStatus& status, std::string& error)
    {
        (void)status;
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

        // Whole-machine map: the build happens inside MemProcFS and cannot be interrupted, so the budget is only
        // enforced before the call and around the copy loop below.
        const DWORD      flags = request.big != 0 ? VMMDLL_POOLMAP_FLAG_BIG : VMMDLL_POOLMAP_FLAG_ALL;
        PVMMDLL_MAP_POOL map   = nullptr;
        const BOOL       ok    = VMMDLL_Map_GetPool(handle, &map, flags);
        const std::unique_ptr<VMMDLL_MAP_POOL, decltype(&VMMDLL_MemFree)> guard {map, &VMMDLL_MemFree};
        if (!ok || map == nullptr)
        {
            error = "VMMDLL_Map_GetPool failed";
            return std::nullopt;
        }

        PoolTagListResult result;
        result.mapTags    = map->cTag;
        result.mapEntries = map->cMap;
        if (map->pTag != nullptr)
        {
            result.tags.reserve(std::min<std::size_t>(map->cTag, kMaxPoolEntries));
            for (DWORD i = 0; i < map->cTag; ++i)
            {
                PoolTagDescriptor summary;
                summary.tag     = map->pTag[i].dwTag;
                summary.entries = map->pTag[i].cEntry;

                if (result.tags.size() == kMaxPoolEntries)
                {
                    result.truncated = true;
                    break;
                }
                if ((i & 0xFFF) == 0 && requestExpired())
                {
                    error = "request deadline exceeded";
                    return std::nullopt;
                }
                if (!matchesPoolTagFilter(request, summary))
                {
                    continue;
                }
                result.tags.push_back(summary);
            }
        }
        return result;
    }
} // namespace mpfshost
