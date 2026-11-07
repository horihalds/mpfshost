#include "host/vmm_heaps.hpp"

#include "core/ipc/frame.hpp"
#include "core/server/request_deadline.hpp"
#include "host/vmm_process_probe.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>

namespace mpfshost
{
    std::optional<HeapListResult>
    listVmmHeaps(VMM_HANDLE handle, const HeapListRequest& request, LookupStatus& status, std::string& error)
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

        PVMMDLL_MAP_HEAP map = nullptr;
        const BOOL       ok  = VMMDLL_Map_GetHeap(handle, request.pid, &map);
        const std::unique_ptr<VMMDLL_MAP_HEAP, decltype(&VMMDLL_MemFree)> guard {map, &VMMDLL_MemFree};
        if (!ok || map == nullptr)
        {
            if (!processPresent(handle, request.pid))
            {
                status = LookupStatus::NotFound;
                error  = "process " + std::to_string(request.pid) + " is not present";
                return std::nullopt;
            }
            error = "VMMDLL_Map_GetHeap failed";
            return std::nullopt;
        }
        if (requestExpired())
        {
            error = "request deadline exceeded";
            return std::nullopt;
        }

        HeapListResult result;
        result.heaps.reserve(std::min<std::size_t>(map->cMap, kMaxHeapEntries));
        for (DWORD i = 0; i < map->cMap; ++i)
        {
            const auto&    entry = map->pMap[i];
            HeapDescriptor heap;
            heap.address = entry.va;
            heap.index   = entry.iHeap;
            heap.number  = entry.dwHeapNum;
            heap.type    = static_cast<std::uint8_t>(entry.tp);
            heap.is32Bit = entry.f32 != 0;

            if (result.heaps.size() == kMaxHeapEntries)
            {
                result.truncated = true;
                break;
            }
            result.heaps.push_back(heap);
        }
        return result;
    }

    std::optional<HeapAllocListResult> listVmmHeapAllocations(VMM_HANDLE                  handle,
                                                              const HeapAllocListRequest& request,
                                                              LookupStatus&               status,
                                                              std::string&                error)
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

        PVMMDLL_MAP_HEAPALLOC map = nullptr;
        // MemProcFS accepts either the heap number or the heap address in the selector.
        const BOOL            ok  = VMMDLL_Map_GetHeapAlloc(handle, request.pid, request.heap, &map);
        const std::unique_ptr<VMMDLL_MAP_HEAPALLOC, decltype(&VMMDLL_MemFree)> guard {map, &VMMDLL_MemFree};
        if (!ok || map == nullptr)
        {
            if (!processPresent(handle, request.pid))
            {
                status = LookupStatus::NotFound;
                error  = "process " + std::to_string(request.pid) + " is not present";
                return std::nullopt;
            }
            error = "VMMDLL_Map_GetHeapAlloc failed";
            return std::nullopt;
        }
        if (requestExpired())
        {
            error = "request deadline exceeded";
            return std::nullopt;
        }

        HeapAllocListResult result;
        result.allocations.reserve(std::min<std::size_t>(map->cMap, kMaxHeapAllocEntries));
        for (DWORD i = 0; i < map->cMap; ++i)
        {
            const auto&         entry = map->pMap[i];
            HeapAllocDescriptor allocation;
            allocation.address = entry.va;
            allocation.size    = entry.cb;
            allocation.type    = static_cast<std::uint8_t>(entry.tp);

            if (result.allocations.size() == kMaxHeapAllocEntries)
            {
                result.truncated = true;
                break;
            }
            result.allocations.push_back(allocation);
        }
        return result;
    }
} // namespace mpfshost
