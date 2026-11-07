#ifndef MPFSHOST_CORE_SERVER_POOL_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_POOL_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    struct PoolListResult
    {
        std::vector<PoolDescriptor> entries;
        std::uint64_t               mapEntries {}; // the allocation count MemProcFS reported, before filtering
        std::uint64_t               mapBytes {};   // cbTotal, the byte cost of the whole map build
        bool                        truncated {};
    };

    struct PoolTagListResult
    {
        std::vector<PoolTagDescriptor> tags;
        std::uint64_t                  mapTags {};    // cTag, the tag count MemProcFS reported, before filtering
        std::uint64_t                  mapEntries {}; // cMap, the allocation count MemProcFS reported
        bool                           truncated {};
    };

    class PoolProvider
    {
    public:
        virtual ~PoolProvider() = default;

        [[nodiscard]] virtual std::optional<PoolListResult>
        listPool(const PoolListRequest& request, LookupStatus& status, std::string& error) = 0;

        [[nodiscard]] virtual std::optional<PoolTagListResult>
        listPoolTags(const PoolTagListRequest& request, LookupStatus& status, std::string& error) = 0;
    };

    [[nodiscard]] bool matchesPoolFilter(const PoolListRequest& request, const PoolDescriptor& entry);
    [[nodiscard]] bool matchesPoolTagFilter(const PoolTagListRequest& request, const PoolTagDescriptor& entry);
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_POOL_PROVIDER_HPP
