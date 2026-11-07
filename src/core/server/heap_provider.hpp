#ifndef MPFSHOST_CORE_SERVER_HEAP_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_HEAP_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    struct HeapListResult
    {
        std::vector<HeapDescriptor> heaps;
        bool                        truncated {};
    };

    class HeapProvider
    {
    public:
        virtual ~HeapProvider() = default;

        [[nodiscard]] virtual std::optional<HeapListResult>
        listHeaps(const HeapListRequest& request, LookupStatus& status, std::string& error) = 0;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_HEAP_PROVIDER_HPP
