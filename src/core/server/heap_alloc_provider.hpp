#ifndef MPFSHOST_CORE_SERVER_HEAP_ALLOC_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_HEAP_ALLOC_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    struct HeapAllocListResult
    {
        std::vector<HeapAllocDescriptor> allocations;
        bool                             truncated {};
    };

    class HeapAllocProvider
    {
    public:
        virtual ~HeapAllocProvider() = default;

        [[nodiscard]] virtual std::optional<HeapAllocListResult>
        listHeapAllocations(const HeapAllocListRequest& request, LookupStatus& status, std::string& error) = 0;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_HEAP_ALLOC_PROVIDER_HPP
