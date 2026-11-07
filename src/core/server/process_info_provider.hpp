#ifndef MPFSHOST_CORE_SERVER_PROCESS_INFO_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_PROCESS_INFO_PROVIDER_HPP
#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <optional>
#include <string>

namespace mpfshost
{
    class ProcessInfoProvider
    {
    public:
        virtual ~ProcessInfoProvider() = default;

        [[nodiscard]] virtual std::optional<ProcessInfoResponse>
        processInfo(const ProcessInfoRequest& request, LookupStatus& status, std::string& error) = 0;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_PROCESS_INFO_PROVIDER_HPP
