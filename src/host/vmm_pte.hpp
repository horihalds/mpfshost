#ifndef MPFSHOST_HOST_VMM_PTE_HPP
#define MPFSHOST_HOST_VMM_PTE_HPP

#pragma once

#include "core/server/pte_provider.hpp"

#include <optional>
#include <string>

#include "vmmdll.h"

namespace mpfshost
{
    [[nodiscard]] std::optional<PteListResult>
    listVmmPte(VMM_HANDLE handle, const PteListRequest& request, LookupStatus& status, std::string& error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_PTE_HPP
