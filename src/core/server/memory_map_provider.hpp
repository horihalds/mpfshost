#ifndef MPFSHOST_CORE_SERVER_MEMORY_MAP_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_MEMORY_MAP_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    class MemoryMapProvider
    {
    public:
        virtual ~MemoryMapProvider() = default;

        [[nodiscard]] virtual std::optional<std::vector<MemoryMapRegion>> listMemoryMap(const MemoryMapRequest& request,
                                                                                        std::string& error) = 0;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_MEMORY_MAP_PROVIDER_HPP
