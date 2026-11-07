#ifndef MPFSHOST_CORE_SERVER_PFN_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_PFN_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    struct PfnListResult
    {
        std::vector<PfnDescriptor> entries;
    };

    class PfnProvider
    {
    public:
        virtual ~PfnProvider() = default;

        [[nodiscard]] virtual std::optional<PfnListResult>
        listPfn(const PfnListRequest& request, LookupStatus& status, std::string& error) = 0;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_PFN_PROVIDER_HPP
