#ifndef MPFSHOST_CORE_SERVER_PAGE_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_PAGE_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mpfshost
{
    class PageReadProvider
    {
    public:
        virtual ~PageReadProvider() = default;

        // One item per address, in order; a page MemProcFS cannot read is an item with ok = false rather than
        // a failure.
        [[nodiscard]] virtual bool readPages(std::uint32_t                  pid,
                                             std::span<const std::uint64_t> addresses,
                                             std::vector<PageReadItem>&     out,
                                             std::string&                   error) = 0;
    };

    class PrefetchProvider
    {
    public:
        virtual ~PrefetchProvider() = default;

        [[nodiscard]] virtual bool
        prefetchPages(std::uint32_t pid, std::span<const std::uint64_t> addresses, std::string& error) = 0;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_PAGE_PROVIDER_HPP
