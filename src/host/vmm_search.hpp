#ifndef MPFSHOST_HOST_VMM_SEARCH_HPP
#define MPFSHOST_HOST_VMM_SEARCH_HPP

#pragma once

#include "core/ipc/message.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "vmmdll.h"

namespace mpfshost
{
    // One VMMDLL_MemSearch call over the request window. A search that exhausted the request budget fails
    // with a deadline error: MemProcFS can only be stopped from inside its result callback, so no partial
    // result is ever returned.
    [[nodiscard]] bool searchVmmMemory(VMM_HANDLE                  handle,
                                       const MemorySearchRequest&  request,
                                       std::vector<std::uint64_t>& hits,
                                       bool&                       truncated,
                                       std::string&                error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_SEARCH_HPP
