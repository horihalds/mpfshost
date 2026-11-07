#ifndef MPFSHOST_HOST_VMM_THREADS_HPP
#define MPFSHOST_HOST_VMM_THREADS_HPP
#pragma once

#include "core/ipc/message.hpp"

#include <optional>
#include <string>
#include <vector>

#include "vmmdll.h"

namespace mpfshost
{
    [[nodiscard]] std::optional<std::vector<ThreadDescriptor>>
    listVmmThreads(VMM_HANDLE handle, const ThreadListRequest& request, std::string& error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_THREADS_HPP
