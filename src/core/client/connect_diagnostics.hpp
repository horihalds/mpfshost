#ifndef MPFSHOST_CORE_CLIENT_CONNECT_DIAGNOSTICS_HPP
#define MPFSHOST_CORE_CLIENT_CONNECT_DIAGNOSTICS_HPP

#pragma once

#include "core/util/identity.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mpfshost
{
    struct SocketFileInfo
    {
        std::uint32_t mode {};
        std::uint32_t ownerUid {};
        std::uint32_t groupGid {};
    };

    struct ConnectDenialContext
    {
        std::filesystem::path         socketPath;
        std::string                   errorText;
        std::optional<SocketFileInfo> file;
        std::uint32_t                 callerUid {};
        std::optional<std::string>    callerName;
        std::vector<std::uint32_t>    callerGroupIds;
        std::optional<GroupRecord>    socketGroup;
    };

    [[nodiscard]] std::optional<SocketFileInfo> readSocketFileInfo(const std::filesystem::path& path);
    [[nodiscard]] ConnectDenialContext          collectConnectDenialContext(const std::filesystem::path& socketPath,
                                                                            std::string_view             errorText);
    [[nodiscard]] std::string                   describeConnectDenial(const ConnectDenialContext& context);
} // namespace mpfshost

#endif // MPFSHOST_CORE_CLIENT_CONNECT_DIAGNOSTICS_HPP
