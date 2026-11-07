#ifndef MPFSHOST_HOST_VMM_HEAPS_HPP
#define MPFSHOST_HOST_VMM_HEAPS_HPP
#pragma once

#include "core/server/heap_alloc_provider.hpp"
#include "core/server/heap_provider.hpp"

#include <optional>
#include <string>

#include "vmmdll.h"

namespace mpfshost
{
    [[nodiscard]] std::optional<HeapListResult>
    listVmmHeaps(VMM_HANDLE handle, const HeapListRequest& request, LookupStatus& status, std::string& error);

    [[nodiscard]] std::optional<HeapAllocListResult> listVmmHeapAllocations(VMM_HANDLE                  handle,
                                                                            const HeapAllocListRequest& request,
                                                                            LookupStatus&               status,
                                                                            std::string&                error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_HEAPS_HPP
