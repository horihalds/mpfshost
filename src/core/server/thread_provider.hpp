#ifndef MPFSHOST_CORE_SERVER_THREAD_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_THREAD_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    class ThreadProvider
    {
    public:
        virtual ~ThreadProvider() = default;

        [[nodiscard]] virtual std::optional<std::vector<ThreadDescriptor>> listThreads(const ThreadListRequest& request,
                                                                                       std::string& error) = 0;
    };

    [[nodiscard]] bool matchesThreadFilter(const ThreadListRequest& request, const ThreadDescriptor& thread);
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_THREAD_PROVIDER_HPP
