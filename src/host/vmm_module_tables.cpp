#include "host/vmm_module_tables.hpp"

#include "core/ipc/frame.hpp"
#include "host/vmm_text.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace mpfshost
{
    namespace
    {
        // Both tables are per-module, so a zero base means the module is not loaded, which is a lookup miss and
        // not a backend failure. It is also the cheap probe MemProcFS offers before either map call allocates.
        bool moduleLoaded(VMM_HANDLE handle, std::uint32_t pid, const std::string& module)
        {
            return VMMDLL_ProcessGetModuleBaseU(handle, pid, module.c_str()) != 0;
        }

        void reportMissingModule(
            VMM_HANDLE handle, std::uint32_t pid, const std::string& module, LookupStatus& status, std::string& error)
        {
            if (!moduleLoaded(handle, pid, module))
            {
                status = LookupStatus::NotFound;
                error  = "module '" + module + "' is not loaded in process " + std::to_string(pid);
                return;
            }
            error = "module table lookup failed";
        }
    } // namespace

    std::optional<ExportListResult>
    listVmmExports(VMM_HANDLE handle, const ExportListRequest& request, LookupStatus& status, std::string& error)
    {
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        PVMMDLL_MAP_EAT map = nullptr;
        const BOOL      ok  = VMMDLL_Map_GetEATU(handle, request.pid, request.module.c_str(), &map);
        const std::unique_ptr<VMMDLL_MAP_EAT, decltype(&VMMDLL_MemFree)> guard {map, &VMMDLL_MemFree};
        if (!ok || map == nullptr)
        {
            reportMissingModule(handle, request.pid, request.module, status, error);
            return std::nullopt;
        }

        ExportListResult result;
        result.exports.reserve(std::min<std::size_t>(map->cMap, kMaxModuleTableEntries));
        for (DWORD i = 0; i < map->cMap; ++i)
        {
            if (result.exports.size() == kMaxModuleTableEntries)
            {
                result.truncated = true;
                break;
            }
            const auto&      entry = map->pMap[i];
            ExportDescriptor descriptor;
            descriptor.vaFunction           = entry.vaFunction;
            descriptor.ordinal              = entry.dwOrdinal;
            descriptor.functionsArrayOffset = entry.oFunctionsArray;
            descriptor.namesArrayOffset     = entry.oNamesArray;
            descriptor.function             = copyMapText(entry.uszFunction);
            descriptor.forwardedFunction    = copyMapText(entry.uszForwardedFunction);
            result.exports.push_back(std::move(descriptor));
        }
        return result;
    }

    std::optional<ImportListResult>
    listVmmImports(VMM_HANDLE handle, const ImportListRequest& request, LookupStatus& status, std::string& error)
    {
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        PVMMDLL_MAP_IAT map = nullptr;
        const BOOL      ok  = VMMDLL_Map_GetIATU(handle, request.pid, request.module.c_str(), &map);
        const std::unique_ptr<VMMDLL_MAP_IAT, decltype(&VMMDLL_MemFree)> guard {map, &VMMDLL_MemFree};
        if (!ok || map == nullptr)
        {
            reportMissingModule(handle, request.pid, request.module, status, error);
            return std::nullopt;
        }

        ImportListResult result;
        result.imports.reserve(std::min<std::size_t>(map->cMap, kMaxModuleTableEntries));
        for (DWORD i = 0; i < map->cMap; ++i)
        {
            if (result.imports.size() == kMaxModuleTableEntries)
            {
                result.truncated = true;
                break;
            }
            const auto&      entry = map->pMap[i];
            ImportDescriptor descriptor;
            descriptor.vaFunction            = entry.vaFunction;
            descriptor.is32Bit               = entry.Thunk.f32 != 0;
            descriptor.hint                  = entry.Thunk.wHint;
            descriptor.firstThunkRva         = entry.Thunk.rvaFirstThunk;
            descriptor.originalFirstThunkRva = entry.Thunk.rvaOriginalFirstThunk;
            descriptor.function              = copyMapText(entry.uszFunction);
            descriptor.module                = copyMapText(entry.uszModule);
            result.imports.push_back(std::move(descriptor));
        }
        return result;
    }
} // namespace mpfshost
