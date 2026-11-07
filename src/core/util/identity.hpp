#ifndef MPFSHOST_CORE_UTIL_IDENTITY_HPP
#define MPFSHOST_CORE_UTIL_IDENTITY_HPP

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    struct GroupRecord
    {
        std::string              name;
        std::uint32_t            gid {};
        std::vector<std::string> members;
    };

    [[nodiscard]] std::uint32_t              currentUid();
    [[nodiscard]] std::vector<std::uint32_t> currentGroupIds();
    [[nodiscard]] std::optional<GroupRecord> readGroupInfo(std::uint32_t gid);
    [[nodiscard]] std::optional<std::string> readUserName(std::uint32_t uid);
} // namespace mpfshost

#endif // MPFSHOST_CORE_UTIL_IDENTITY_HPP
