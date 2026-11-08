#ifndef MPFSHOST_HOST_VMM_PROCESSES_HPP
#define MPFSHOST_HOST_VMM_PROCESSES_HPP
#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <optional>
#include <string>
#include <vector>

#include "vmmdll.h"

namespace mpfshost
{
    struct ProcessDtbRecord
    {
        ProcessDescriptor process;
        std::uint64_t     dtb {};
        std::uint64_t     dtbUser {};
    };

    [[nodiscard]] std::optional<std::vector<ProcessDescriptor>> listVmmProcesses(VMM_HANDLE handle, std::string& error);

    [[nodiscard]] std::optional<std::vector<ProcessSummary>> listAllVmmProcesses(VMM_HANDLE handle, std::string& error);

    [[nodiscard]] std::optional<std::vector<ProcessDtbRecord>> listVmmProcessDtbs(VMM_HANDLE   handle,
                                                                                  std::string& error);

    [[nodiscard]] std::optional<ProcessInfoResponse>
    queryVmmProcess(VMM_HANDLE handle, const ProcessInfoRequest& request, LookupStatus& status, std::string& error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_PROCESSES_HPP
