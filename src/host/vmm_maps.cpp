#include "host/vmm_maps.hpp"

#include "core/ipc/frame.hpp"
#include "core/server/request_deadline.hpp"
#include "host/vmm_process_probe.hpp"
#include "host/vmm_text.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mpfshost
{
    namespace
    {
        struct ImageModule
        {
            std::uint64_t base;
            std::uint64_t size;
            std::string   name;
        };

        // MemProcFS only writes VAD text for TEB, stack, heap and section regions, so image regions carry no
        // description even when module identification is asked for; the process module map fills that gap.
        std::vector<ImageModule> imageModules(VMM_HANDLE handle, std::uint32_t pid)
        {
            PVMMDLL_MAP_MODULE map = nullptr;
            const BOOL         ok  = VMMDLL_Map_GetModuleU(handle, pid, &map, 0);
            const std::unique_ptr<VMMDLL_MAP_MODULE, decltype(&VMMDLL_MemFree)> guard {map, &VMMDLL_MemFree};

            std::vector<ImageModule> modules;
            if (!ok || map == nullptr)
            {
                return modules;
            }
            modules.reserve(map->cMap);
            for (DWORD i = 0; i < map->cMap; ++i)
            {
                const auto& entry = map->pMap[i];
                modules.push_back({entry.vaBase, entry.cbImageSize, copyMapText(entry.uszText)});
            }
            return modules;
        }

        std::string imageModuleName(const std::vector<ImageModule>& modules, std::uint64_t address)
        {
            for (const auto& module : modules)
            {
                if (address < module.base)
                {
                    continue;
                }
                if (module.size == 0 ? address == module.base : address < module.base + module.size)
                {
                    return module.name;
                }
            }
            return {};
        }

        std::uint16_t regionFlags(const VMMDLL_MAP_VADENTRY& entry)
        {
            std::uint16_t flags = 0;
            if (entry.fImage != 0)
            {
                flags |= kMemoryMapFlagImage;
            }
            if (entry.fFile != 0)
            {
                flags |= kMemoryMapFlagFile;
            }
            if (entry.fPageFile != 0)
            {
                flags |= kMemoryMapFlagPageFile;
            }
            if (entry.fPrivateMemory != 0)
            {
                flags |= kMemoryMapFlagPrivate;
            }
            if (entry.fTeb != 0)
            {
                flags |= kMemoryMapFlagTeb;
            }
            if (entry.fStack != 0)
            {
                flags |= kMemoryMapFlagStack;
            }
            if (entry.fHeap != 0)
            {
                flags |= kMemoryMapFlagHeap;
            }
            return flags;
        }
    } // namespace

    std::optional<std::vector<MemoryMapRegion>>
    listVmmMemoryMap(VMM_HANDLE handle, const MemoryMapRequest& request, std::string& error)
    {
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        PVMMDLL_MAP_VAD map = nullptr;
        const BOOL      ok  = VMMDLL_Map_GetVadU(handle, request.pid, request.identifyModules ? 1 : 0, &map);
        const std::unique_ptr<VMMDLL_MAP_VAD, decltype(&VMMDLL_MemFree)> guard {map, &VMMDLL_MemFree};
        if (!ok || map == nullptr)
        {
            error = "VMMDLL_Map_GetVadU failed";
            return std::nullopt;
        }

        std::vector<MemoryMapRegion> regions;
        regions.reserve(map->cMap);
        for (DWORD i = 0; i < map->cMap; ++i)
        {
            const auto&     entry = map->pMap[i];
            MemoryMapRegion region;
            region.start        = entry.vaStart;
            region.end          = entry.vaEnd;
            region.protection   = entry.Protection;
            region.commitCharge = entry.CommitCharge;
            region.vadType      = static_cast<std::uint16_t>(entry.VadType);
            region.flags        = regionFlags(entry);
            region.heapNumber   = static_cast<std::uint8_t>(entry.HeapNum);
            region.description  = copyMapText(entry.uszText);
            regions.push_back(std::move(region));
        }

        if (request.identifyModules)
        {
            const auto modules = imageModules(handle, request.pid);
            for (auto& region : regions)
            {
                if (region.description.empty() && (region.flags & kMemoryMapFlagImage) != 0)
                {
                    region.description = imageModuleName(modules, region.start);
                }
            }
        }
        return regions;
    }

    std::optional<std::vector<VadExDescriptor>>
    listVmmVadEx(VMM_HANDLE handle, const VadExListRequest& request, LookupStatus& status, std::string& error)
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

        PVMMDLL_MAP_VADEX map = nullptr;
        const BOOL        ok  = VMMDLL_Map_GetVadEx(handle, request.pid, request.pageOffset, request.pageCount, &map);
        const std::unique_ptr<VMMDLL_MAP_VADEX, decltype(&VMMDLL_MemFree)> guard {map, &VMMDLL_MemFree};
        if (!ok || map == nullptr)
        {
            if (!processPresent(handle, request.pid))
            {
                status = LookupStatus::NotFound;
                error  = "process " + std::to_string(request.pid) + " is not present";
                return std::nullopt;
            }
            error = "VMMDLL_Map_GetVadEx failed";
            return std::nullopt;
        }
        if (requestExpired())
        {
            error = "request deadline exceeded";
            return std::nullopt;
        }

        std::vector<VadExDescriptor> entries;
        entries.reserve(map->cMap);
        for (DWORD i = 0; i < map->cMap; ++i)
        {
            const auto&     entry = map->pMap[i];
            VadExDescriptor page;
            page.type      = static_cast<std::uint32_t>(entry.tp);
            page.pml       = entry.iPML;
            page.flags     = entry.pteFlags;
            page.va        = entry.va;
            page.pa        = entry.pa;
            page.pte       = entry.pte;
            page.protoType = static_cast<std::uint32_t>(entry.proto.tp);
            page.protoPa   = entry.proto.pa;
            page.protoPte  = entry.proto.pte;
            page.vaVadBase = entry.vaVadBase;
            entries.push_back(page);
        }
        return entries;
    }
} // namespace mpfshost
