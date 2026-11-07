#ifndef MPFSHOST_HOST_VMM_MODULE_TABLES_HPP
#define MPFSHOST_HOST_VMM_MODULE_TABLES_HPP
#pragma once

#include "core/server/module_table_provider.hpp"

#include <optional>
#include <string>

#include "vmmdll.h"

namespace mpfshost
{
    [[nodiscard]] std::optional<ExportListResult>
    listVmmExports(VMM_HANDLE handle, const ExportListRequest& request, LookupStatus& status, std::string& error);

    [[nodiscard]] std::optional<ImportListResult>
    listVmmImports(VMM_HANDLE handle, const ImportListRequest& request, LookupStatus& status, std::string& error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_MODULE_TABLES_HPP
