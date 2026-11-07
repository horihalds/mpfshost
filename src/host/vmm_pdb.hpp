#ifndef MPFSHOST_HOST_VMM_PDB_HPP
#define MPFSHOST_HOST_VMM_PDB_HPP
#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <optional>
#include <string>

#include "vmmdll.h"

namespace mpfshost
{
    [[nodiscard]] std::optional<PdbLoadResponse>
    loadVmmPdb(VMM_HANDLE handle, const PdbLoadRequest& request, LookupStatus& status, std::string& error);

    [[nodiscard]] std::optional<PdbSymbolNameResponse> lookupVmmPdbSymbolName(VMM_HANDLE                  handle,
                                                                              const PdbSymbolNameRequest& request,
                                                                              LookupStatus&               status,
                                                                              std::string&                error);

    [[nodiscard]] std::optional<PdbSymbolAddressResponse> lookupVmmPdbSymbolAddress(
        VMM_HANDLE handle, const PdbSymbolAddressRequest& request, LookupStatus& status, std::string& error);

    [[nodiscard]] std::optional<PdbTypeSizeResponse> lookupVmmPdbTypeSize(VMM_HANDLE                handle,
                                                                          const PdbTypeSizeRequest& request,
                                                                          LookupStatus&             status,
                                                                          std::string&              error);

    [[nodiscard]] std::optional<PdbTypeChildOffsetResponse> lookupVmmPdbTypeChildOffset(
        VMM_HANDLE handle, const PdbTypeChildOffsetRequest& request, LookupStatus& status, std::string& error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_PDB_HPP
