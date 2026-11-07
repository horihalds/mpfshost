#ifndef MPFSHOST_CORE_SERVER_CONFIG_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_CONFIG_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"

#include <string>

namespace mpfshost
{
    class ConfigProvider
    {
    public:
        virtual ~ConfigProvider() = default;

        [[nodiscard]] virtual bool setConfig(const ConfigSetRequest& request, std::string& error) = 0;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_CONFIG_PROVIDER_HPP
