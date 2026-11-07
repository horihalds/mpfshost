#ifndef MPFSHOST_CORE_SERVER_VIRTUAL_QUERY_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_VIRTUAL_QUERY_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <optional>
#include <string>

namespace mpfshost
{
    class VirtualQueryProvider
    {
    public:
        virtual ~VirtualQueryProvider() = default;

        // Nothing with status NotFound when the address belongs to no region; a failure with Ok means the
        // query itself could not be answered.
        [[nodiscard]] virtual std::optional<VirtualRegion>
        queryVirtual(const VirtualQueryRequest& request, LookupStatus& status, std::string& error) = 0;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_VIRTUAL_QUERY_PROVIDER_HPP
