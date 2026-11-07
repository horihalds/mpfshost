#ifndef MPFSHOST_CORE_SERVER_MODULE_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_MODULE_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    class ModuleProvider
    {
    public:
        virtual ~ModuleProvider() = default;

        [[nodiscard]] virtual std::optional<std::vector<ModuleDescriptor>> listModules(const ModuleListRequest& request,
                                                                                       std::string& error) = 0;
    };

    [[nodiscard]] bool matchesModuleFilter(const ModuleListRequest& request, const ModuleDescriptor& module);
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_MODULE_PROVIDER_HPP
