#ifndef MPFSHOST_CORE_SERVER_MEMORY_SEARCH_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_MEMORY_SEARCH_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mpfshost
{
    class MemorySearchProvider
    {
    public:
        virtual ~MemorySearchProvider() = default;

        // Hits in ascending order, at most `request.maxResults` of them; `truncated` says whether the cap
        // ended the scan rather than the end of the searched window.
        [[nodiscard]] virtual bool searchMemory(const MemorySearchRequest&  request,
                                                std::vector<std::uint64_t>& hits,
                                                bool&                       truncated,
                                                std::string&                error) = 0;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_MEMORY_SEARCH_PROVIDER_HPP
