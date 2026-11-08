#ifndef MPFSHOST_CORE_SERVER_MODULE_LOOKUP_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_MODULE_LOOKUP_PROVIDER_HPP
#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

        [[nodiscard]] virtual std::optional<ModuleByAddressResponse>
        moduleByAddress(const ModuleByAddressRequest& request, LookupStatus& status, std::string& error) = 0;
    };

    // Returns the module that contains `address`, or nullptr. Modules report an image size, so the upper bound is
    // exclusive; the subtraction form cannot wrap when a caller probes near the top of the address space.
    [[nodiscard]] const ModuleDescriptor* findModuleAtAddress(const std::vector<ModuleDescriptor>& modules,
                                                              std::uint64_t                        address) noexcept;
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_MODULE_LOOKUP_PROVIDER_HPP
