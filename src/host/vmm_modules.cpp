#include "host/vmm_modules.hpp"

#include "core/ipc/frame.hpp"
#include "host/vmm_text.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace mpfshost
{
    std::optional<std::vector<ModuleDescriptor>>
    listVmmModules(VMM_HANDLE handle, const ModuleListRequest& request, std::string& error)
    {
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        DWORD flags = 0;
        if ((request.flags & kModuleFlagDebugInfo) != 0)
        {
            flags |= VMMDLL_MODULE_FLAG_DEBUGINFO;
        }
        if ((request.flags & kModuleFlagVersionInfo) != 0)
        {
            flags |= VMMDLL_MODULE_FLAG_VERSIONINFO;
        }

        PVMMDLL_MAP_MODULE map = nullptr;
        const BOOL         ok  = VMMDLL_Map_GetModuleU(handle, request.pid, &map, flags);
        const std::unique_ptr<VMMDLL_MAP_MODULE, decltype(&VMMDLL_MemFree)> guard {map, &VMMDLL_MemFree};
        if (!ok || map == nullptr)
        {
            error = "VMMDLL_Map_GetModuleU failed";
            return std::nullopt;
        }

        std::vector<ModuleDescriptor> modules;
        modules.reserve(map->cMap);
        for (DWORD i = 0; i < map->cMap; ++i)
        {
            const auto&      entry = map->pMap[i];
            ModuleDescriptor descriptor;
            descriptor.base      = entry.vaBase;
            descriptor.entry     = entry.vaEntry;
            descriptor.imageSize = entry.cbImageSize;
            descriptor.kind      = static_cast<std::uint16_t>(entry.tp);
            descriptor.wow64     = entry.fWoW64 != 0;
            descriptor.name      = copyMapText(entry.uszText);
            descriptor.fullName  = copyMapText(entry.uszFullName);

            if ((request.flags & kModuleFlagCounts) != 0)
            {
                descriptor.rawFileSize  = entry.cbFileSizeRaw;
                descriptor.sectionCount = static_cast<std::uint16_t>(entry.cSection);
                descriptor.eatCount     = entry.cEAT;
                descriptor.iatCount     = entry.cIAT;
            }
            if ((request.flags & kModuleFlagDebugInfo) != 0 && entry.pExDebugInfo != nullptr)
            {
                descriptor.pdbGuid = copyMapText(entry.pExDebugInfo->uszGuid);
                descriptor.pdbName = copyMapText(entry.pExDebugInfo->uszPdbFilename);
            }
            if ((request.flags & kModuleFlagVersionInfo) != 0 && entry.pExVersionInfo != nullptr)
            {
                descriptor.version = copyMapText(entry.pExVersionInfo->uszFileVersion);
            }
            modules.push_back(std::move(descriptor));
        }
        return modules;
    }

    std::optional<SymbolLookupResponse>
    lookupVmmSymbol(VMM_HANDLE handle, const SymbolLookupRequest& request, LookupStatus& status, std::string& error)
    {
        status = LookupStatus::Failed;
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }
        if (request.module.empty())
        {
            error = "a symbol lookup needs a module name";
            return std::nullopt;
        }

        SymbolLookupResponse response;
        response.moduleBase = VMMDLL_ProcessGetModuleBaseU(handle, request.pid, request.module.c_str());
        if (response.moduleBase == 0)
        {
            status = LookupStatus::NotFound;
            error  = "module '" + request.module + "' is not loaded in process " + std::to_string(request.pid);
            return std::nullopt;
        }

        if (!request.symbol.empty())
        {
            const auto address =
                VMMDLL_ProcessGetProcAddressU(handle, request.pid, request.module.c_str(), request.symbol.c_str());
            // A zero result means the export is absent; it cannot be confused with a real address because a
            // symbol never sits below the module base.
            if (address != 0)
            {
                response.address = address;
                response.offset  = address - response.moduleBase;
                response.found   = true;
            }
        }

        status = LookupStatus::Ok;
        return response;
    }

    std::optional<ModuleLayoutResponse> queryVmmModuleLayout(VMM_HANDLE                 handle,
                                                             const ModuleLayoutRequest& request,
                                                             LookupStatus&              status,
                                                             std::string&               error)
    {
        status = LookupStatus::Failed;
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }
        if (request.module.empty())
        {
            error = "a module layout query needs a module name";
            return std::nullopt;
        }

        ModuleLayoutResponse response;
        response.moduleBase = VMMDLL_ProcessGetModuleBaseU(handle, request.pid, request.module.c_str());
        if (response.moduleBase == 0)
        {
            status = LookupStatus::NotFound;
            error  = "module '" + request.module + "' is not loaded in process " + std::to_string(request.pid);
            return std::nullopt;
        }

        // MemProcFS only fills the caller buffer when cSections equals the module's section count exactly, so the
        // count comes from a first call with a null buffer. Asking with any other size returns failure.
        DWORD sectionCount = 0;
        if (!VMMDLL_ProcessGetSectionsU(handle, request.pid, request.module.c_str(), nullptr, 0, &sectionCount))
        {
            error = "VMMDLL_ProcessGetSectionsU failed for module '" + request.module + "'";
            return std::nullopt;
        }
        if (sectionCount > kMaxModuleSections)
        {
            error = "module '" + request.module + "' reports " + std::to_string(sectionCount)
                  + " sections, more than the " + std::to_string(kMaxModuleSections) + " supported";
            return std::nullopt;
        }

        std::array<IMAGE_SECTION_HEADER, kMaxModuleSections> headers {};
        if (sectionCount != 0
            && !VMMDLL_ProcessGetSectionsU(
                handle, request.pid, request.module.c_str(), headers.data(), sectionCount, &sectionCount))
        {
            error = "VMMDLL_ProcessGetSectionsU failed for module '" + request.module + "'";
            return std::nullopt;
        }

        response.sections.reserve(sectionCount);
        for (DWORD i = 0; i < sectionCount; ++i)
        {
            const auto&   header = headers[i];
            ModuleSection section;
            // IMAGE_SECTION_HEADER::Name is a fixed 8-byte field that need not be NUL-terminated.
            section.name = std::string {reinterpret_cast<const char*>(header.Name),
                                        strnlen(reinterpret_cast<const char*>(header.Name), IMAGE_SIZEOF_SHORT_NAME)};
            section.virtualAddress  = response.moduleBase + header.VirtualAddress;
            section.virtualSize     = header.Misc.VirtualSize;
            section.rawPointer      = header.PointerToRawData;
            section.rawSize         = header.SizeOfRawData;
            section.characteristics = header.Characteristics;
            response.sections.push_back(std::move(section));
        }

        std::array<IMAGE_DATA_DIRECTORY, kModuleDataDirectoryCount> directories {};
        if (!VMMDLL_ProcessGetDirectoriesU(handle, request.pid, request.module.c_str(), directories.data()))
        {
            error = "VMMDLL_ProcessGetDirectoriesU failed for module '" + request.module + "'";
            return std::nullopt;
        }
        response.directories.reserve(directories.size());
        for (std::size_t i = 0; i < directories.size(); ++i)
        {
            response.directories.push_back(
                {static_cast<std::uint32_t>(i), directories[i].VirtualAddress, directories[i].Size});
        }

        status = LookupStatus::Ok;
        return response;
    }
} // namespace mpfshost
