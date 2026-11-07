#ifndef MPFSHOST_CORE_SERVER_VAD_EX_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_VAD_EX_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    class VadExProvider
    {
    public:
        virtual ~VadExProvider() = default;

        [[nodiscard]] virtual std::optional<std::vector<VadExDescriptor>>
        listVadEx(const VadExListRequest& request, LookupStatus& status, std::string& error) = 0;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_VAD_EX_PROVIDER_HPP
