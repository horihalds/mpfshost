#include "core/util/identity.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <vector>

#include <grp.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

namespace mpfshost
{
    namespace
    {
        constexpr std::size_t kMaxLookupBufferSize = 1U << 20U;

        std::size_t initialLookupBufferSize(long suggested)
        {
            if (suggested <= 0)
            {
                return 1024;
            }
            return static_cast<std::size_t>(suggested);
        }
    } // namespace

    std::uint32_t currentUid()
    {
        return static_cast<std::uint32_t>(::geteuid());
    }

    std::vector<std::uint32_t> currentGroupIds()
    {
        std::vector<std::uint32_t> groups;
        const auto                 add = [&groups](gid_t gid)
        {
            const auto value = static_cast<std::uint32_t>(gid);
            if (std::find(groups.begin(), groups.end(), value) == groups.end())
            {
                groups.push_back(value);
            }
        };

        // A newgrp or sg shell changes the effective gid without adding it to the supplementary list.
        add(::getegid());
        add(::getgid());

        const int count = ::getgroups(0, nullptr);
        if (count > 0)
        {
            std::vector<gid_t> supplementary(static_cast<std::size_t>(count));
            const int          stored = ::getgroups(count, supplementary.data());
            if (stored > 0)
            {
                for (int index = 0; index < stored; ++index)
                {
                    add(supplementary[static_cast<std::size_t>(index)]);
                }
            }
        }
        return groups;
    }

    std::optional<GroupRecord> readGroupInfo(std::uint32_t gid)
    {
        std::vector<char> buffer(initialLookupBufferSize(::sysconf(_SC_GETGR_R_SIZE_MAX)));
        group             entry {};
        group*            result = nullptr;
        while (true)
        {
            result           = nullptr;
            const int status = ::getgrgid_r(static_cast<gid_t>(gid), &entry, buffer.data(), buffer.size(), &result);
            if (status == 0)
            {
                break;
            }
            if (status != ERANGE || buffer.size() >= kMaxLookupBufferSize)
            {
                return std::nullopt;
            }
            buffer.resize(buffer.size() * 2);
        }
        if (result == nullptr)
        {
            return std::nullopt;
        }

        GroupRecord record;
        if (result->gr_name != nullptr)
        {
            record.name = result->gr_name;
        }
        record.gid = static_cast<std::uint32_t>(result->gr_gid);
        if (result->gr_mem != nullptr)
        {
            for (char** member = result->gr_mem; *member != nullptr; ++member)
            {
                record.members.emplace_back(*member);
            }
        }
        return record;
    }

    std::optional<std::string> readUserName(std::uint32_t uid)
    {
        std::vector<char> buffer(initialLookupBufferSize(::sysconf(_SC_GETPW_R_SIZE_MAX)));
        passwd            entry {};
        passwd*           result = nullptr;
        while (true)
        {
            result           = nullptr;
            const int status = ::getpwuid_r(static_cast<uid_t>(uid), &entry, buffer.data(), buffer.size(), &result);
            if (status == 0)
            {
                break;
            }
            if (status != ERANGE || buffer.size() >= kMaxLookupBufferSize)
            {
                return std::nullopt;
            }
            buffer.resize(buffer.size() * 2);
        }
        if (result == nullptr || result->pw_name == nullptr)
        {
            return std::nullopt;
        }
        return std::string {result->pw_name};
    }
} // namespace mpfshost
