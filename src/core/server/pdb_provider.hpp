#ifndef MPFSHOST_CORE_SERVER_PDB_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_PDB_PROVIDER_HPP
#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <optional>
#include <string>

namespace mpfshost
{
    class PdbProvider
    {
    public:
        virtual ~PdbProvider() = default;

        [[nodiscard]] virtual std::optional<PdbLoadResponse>
        pdbLoad(const PdbLoadRequest& request, LookupStatus& status, std::string& error) = 0;

        [[nodiscard]] virtual std::optional<PdbSymbolNameResponse>
        pdbSymbolName(const PdbSymbolNameRequest& request, LookupStatus& status, std::string& error) = 0;

        [[nodiscard]] virtual std::optional<PdbSymbolAddressResponse>
        pdbSymbolAddress(const PdbSymbolAddressRequest& request, LookupStatus& status, std::string& error) = 0;

        [[nodiscard]] virtual std::optional<PdbTypeSizeResponse>
        pdbTypeSize(const PdbTypeSizeRequest& request, LookupStatus& status, std::string& error) = 0;

        [[nodiscard]] virtual std::optional<PdbTypeChildOffsetResponse>
        pdbTypeChildOffset(const PdbTypeChildOffsetRequest& request, LookupStatus& status, std::string& error) = 0;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_PDB_PROVIDER_HPP
