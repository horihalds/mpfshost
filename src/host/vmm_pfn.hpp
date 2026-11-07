#ifndef MPFSHOST_HOST_VMM_PFN_HPP
#define MPFSHOST_HOST_VMM_PFN_HPP

#pragma once

#include "core/server/pfn_provider.hpp"

#include <optional>
#include <string>

#include "vmmdll.h"

namespace mpfshost
{
    [[nodiscard]] std::optional<PfnListResult>
    listVmmPfn(VMM_HANDLE handle, const PfnListRequest& request, LookupStatus& status, std::string& error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_PFN_HPP
