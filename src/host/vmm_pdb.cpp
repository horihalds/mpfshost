#include "host/vmm_pdb.hpp"

#include <array>
#include <cstdio>
#include <cstring>

namespace mpfshost
{
    namespace
    {
        std::string copyPathName(const std::array<char, MAX_PATH>& buffer)
        {
            return std::string {buffer.data(), strnlen(buffer.data(), buffer.size())};
        }
    } // namespace

    std::optional<PdbLoadResponse>
    loadVmmPdb(VMM_HANDLE handle, const PdbLoadRequest& request, LookupStatus& status, std::string& error)
    {
        status = LookupStatus::Failed;
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        std::array<char, MAX_PATH> moduleName {};
        if (!VMMDLL_PdbLoad(handle, request.pid, request.moduleBase, moduleName.data()))
        {
            char address[32] {};
            std::snprintf(address, sizeof(address), "0x%llx", static_cast<unsigned long long>(request.moduleBase));
            status = LookupStatus::NotFound;
            error  = std::string {"no pdb could be loaded for the module at "} + address;
            return std::nullopt;
        }

        status = LookupStatus::Ok;
        return PdbLoadResponse {copyPathName(moduleName)};
    }

    std::optional<PdbSymbolNameResponse> lookupVmmPdbSymbolName(VMM_HANDLE                  handle,
                                                                const PdbSymbolNameRequest& request,
                                                                LookupStatus&               status,
                                                                std::string&                error)
    {
        status = LookupStatus::Failed;
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        std::array<char, MAX_PATH> symbolName {};
        DWORD                      displacement = 0;
        if (!VMMDLL_PdbSymbolName(handle, request.module.c_str(), request.address, symbolName.data(), &displacement))
        {
            status = LookupStatus::NotFound;
            error  = "no pdb symbol matches the address in module '" + request.module + "'";
            return std::nullopt;
        }

        status = LookupStatus::Ok;
        return PdbSymbolNameResponse {copyPathName(symbolName), displacement};
    }

    std::optional<PdbSymbolAddressResponse> lookupVmmPdbSymbolAddress(VMM_HANDLE                     handle,
                                                                      const PdbSymbolAddressRequest& request,
                                                                      LookupStatus&                  status,
                                                                      std::string&                   error)
    {
        status = LookupStatus::Failed;
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        ULONG64 address = 0;
        if (!VMMDLL_PdbSymbolAddress(handle, request.module.c_str(), request.symbol.c_str(), &address))
        {
            status = LookupStatus::NotFound;
            error  = "no pdb symbol named '" + request.symbol + "' in module '" + request.module + "'";
            return std::nullopt;
        }

        status = LookupStatus::Ok;
        return PdbSymbolAddressResponse {address};
    }

    std::optional<PdbTypeSizeResponse>
    lookupVmmPdbTypeSize(VMM_HANDLE handle, const PdbTypeSizeRequest& request, LookupStatus& status, std::string& error)
    {
        status = LookupStatus::Failed;
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        DWORD size = 0;
        if (!VMMDLL_PdbTypeSize(handle, request.module.c_str(), request.type.c_str(), &size))
        {
            status = LookupStatus::NotFound;
            error  = "no pdb type named '" + request.type + "' in module '" + request.module + "'";
            return std::nullopt;
        }

        status = LookupStatus::Ok;
        return PdbTypeSizeResponse {size};
    }

    std::optional<PdbTypeChildOffsetResponse> lookupVmmPdbTypeChildOffset(VMM_HANDLE                       handle,
                                                                          const PdbTypeChildOffsetRequest& request,
                                                                          LookupStatus&                    status,
                                                                          std::string&                     error)
    {
        status = LookupStatus::Failed;
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        DWORD offset = 0;
        if (!VMMDLL_PdbTypeChildOffset(
                handle, request.module.c_str(), request.type.c_str(), request.child.c_str(), &offset))
        {
            status = LookupStatus::NotFound;
            error  = "no child named '" + request.child + "' in pdb type '" + request.type + "' of module '"
                   + request.module + "'";
            return std::nullopt;
        }

        status = LookupStatus::Ok;
        return PdbTypeChildOffsetResponse {offset};
    }
} // namespace mpfshost
