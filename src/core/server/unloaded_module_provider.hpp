#ifndef MPFSHOST_CORE_SERVER_UNLOADED_MODULE_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_UNLOADED_MODULE_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    struct UnloadedModuleListResult
    {
        std::vector<UnloadedModuleDescriptor> modules;
        bool                                  truncated {};
    };

    class UnloadedModuleProvider
    {
    public:
        virtual ~UnloadedModuleProvider() = default;

        [[nodiscard]] virtual std::optional<UnloadedModuleListResult>
        listUnloadedModules(const UnloadedModuleListRequest& request, LookupStatus& status, std::string& error) = 0;
    };

    [[nodiscard]] bool matchesUnloadedModuleFilter(const UnloadedModuleListRequest& request,
                                                   const UnloadedModuleDescriptor&  module);
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_UNLOADED_MODULE_PROVIDER_HPP
