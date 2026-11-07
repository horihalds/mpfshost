#ifndef MPFSHOST_CORE_SERVER_MODULE_LOOKUP_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_MODULE_LOOKUP_PROVIDER_HPP
#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <optional>
#include <string>

namespace mpfshost
{
    class ModuleLookupProvider
    {
    public:
        virtual ~ModuleLookupProvider() = default;

        [[nodiscard]] virtual std::optional<SymbolLookupResponse>
        lookupSymbol(const SymbolLookupRequest& request, LookupStatus& status, std::string& error) = 0;

        [[nodiscard]] virtual std::optional<ModuleLayoutResponse>
        moduleLayout(const ModuleLayoutRequest& request, LookupStatus& status, std::string& error) = 0;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_MODULE_LOOKUP_PROVIDER_HPP
