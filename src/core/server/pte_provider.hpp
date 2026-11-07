#ifndef MPFSHOST_CORE_SERVER_PTE_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_PTE_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    struct PteListResult
    {
        std::vector<PteDescriptor> entries;
        std::uint64_t              mapEntries {}; // the region count MemProcFS reported, before filtering
        bool                       truncated {};
    };

    class PteProvider
    {
    public:
        virtual ~PteProvider() = default;

        [[nodiscard]] virtual std::optional<PteListResult>
        listPte(const PteListRequest& request, LookupStatus& status, std::string& error) = 0;
    };

    [[nodiscard]] bool matchesPteFilter(const PteListRequest& request, const PteDescriptor& entry);
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_PTE_PROVIDER_HPP
