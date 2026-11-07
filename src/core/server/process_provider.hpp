#ifndef MPFSHOST_CORE_SERVER_PROCESS_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_PROCESS_PROVIDER_HPP
#pragma once

#include "core/ipc/message.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    class ProcessProvider
    {
    public:
        virtual ~ProcessProvider() = default;

        [[nodiscard]] virtual std::optional<std::vector<ProcessDescriptor>> listProcesses(const ProcessFilter& filter,
                                                                                          std::string& error) = 0;

        [[nodiscard]] virtual std::optional<std::vector<ProcessSummary>> listAllProcesses(const ProcessFilter& filter,
                                                                                          std::string& error) = 0;
    };

    [[nodiscard]] bool matchesProcessFilter(const ProcessFilter& filter, const ProcessDescriptor& process);
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_PROCESS_PROVIDER_HPP
