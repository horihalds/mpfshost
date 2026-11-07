#ifndef MPFSHOST_CORE_SERVER_MODULE_TABLE_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_MODULE_TABLE_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    struct ExportListResult
    {
        std::vector<ExportDescriptor> exports;
        bool                          truncated {};
    };

    struct ImportListResult
    {
        std::vector<ImportDescriptor> imports;
        bool                          truncated {};
    };

    class ModuleTableProvider
    {
    public:
        virtual ~ModuleTableProvider() = default;

        [[nodiscard]] virtual std::optional<ExportListResult>
        listExports(const ExportListRequest& request, LookupStatus& status, std::string& error) = 0;

        [[nodiscard]] virtual std::optional<ImportListResult>
        listImports(const ImportListRequest& request, LookupStatus& status, std::string& error) = 0;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_MODULE_TABLE_PROVIDER_HPP
