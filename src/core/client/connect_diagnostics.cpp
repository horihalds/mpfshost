#include "core/client/connect_diagnostics.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>

#include <sys/stat.h>

namespace mpfshost
{
    namespace
    {
        constexpr std::uint32_t kPermissionBits = 07777U;

        std::string formatMode(std::uint32_t mode)
        {
            char buffer[8] {};
            std::snprintf(buffer, sizeof(buffer), "%04o", static_cast<unsigned>(mode));
            return buffer;
        }

        std::string formatGroupIds(const std::vector<std::uint32_t>& ids)
        {
            std::string text = "[";
            for (std::size_t index = 0; index < ids.size(); ++index)
            {
                if (index > 0)
                {
                    text += ", ";
                }
                text += std::to_string(ids[index]);
            }
            text += ']';
            return text;
        }

        bool holdsGroup(const std::vector<std::uint32_t>& ids, std::uint32_t gid)
        {
            return std::find(ids.begin(), ids.end(), gid) != ids.end();
        }

        bool listsMember(const GroupRecord& group, const std::string& name)
        {
            return std::find(group.members.begin(), group.members.end(), name) != group.members.end();
        }
    } // namespace

    std::optional<SocketFileInfo> readSocketFileInfo(const std::filesystem::path& path)
    {
        struct stat status {};
        if (::lstat(path.c_str(), &status) != 0)
        {
            return std::nullopt;
        }

        SocketFileInfo info;
        info.mode     = static_cast<std::uint32_t>(status.st_mode) & kPermissionBits;
        info.ownerUid = static_cast<std::uint32_t>(status.st_uid);
        info.groupGid = static_cast<std::uint32_t>(status.st_gid);
        return info;
    }

    ConnectDenialContext collectConnectDenialContext(const std::filesystem::path& socketPath,
                                                     std::string_view             errorText)
    {
        ConnectDenialContext context;
        context.socketPath     = socketPath;
        context.errorText      = std::string {errorText};
        context.file           = readSocketFileInfo(socketPath);
        context.callerUid      = currentUid();
        context.callerName     = readUserName(context.callerUid);
        context.callerGroupIds = currentGroupIds();
        if (context.file)
        {
            context.socketGroup = readGroupInfo(context.file->groupGid);
        }
        return context;
    }

    std::string describeConnectDenial(const ConnectDenialContext& context)
    {
        std::string message = "connect failed: " + context.errorText;

        if (!context.file)
        {
            message += " (" + context.socketPath.string() + ')';
            return message;
        }

        const auto& file  = *context.file;
        const bool  named = context.socketGroup && !context.socketGroup->name.empty();
        message += " (" + context.socketPath.string() + " is mode " + formatMode(file.mode) + ", owner uid "
                 + std::to_string(file.ownerUid) + ", group ";
        if (named)
        {
            message += '\'' + context.socketGroup->name + "' gid " + std::to_string(file.groupGid);
        }
        else
        {
            message += "gid " + std::to_string(file.groupGid);
        }
        message += ')';

        if (holdsGroup(context.callerGroupIds, file.groupGid))
        {
            message += "; this session already holds gid " + std::to_string(file.groupGid)
                     + ", so the denial comes from elsewhere (check the permissions of the socket's directory)";
            return message;
        }

        message += "; this session's group ids " + formatGroupIds(context.callerGroupIds) + " do not include "
                 + std::to_string(file.groupGid);
        if (!named)
        {
            return message;
        }

        const bool callerKnown = context.callerName && !context.callerName->empty();
        const auto caller      = callerKnown ? *context.callerName : std::string {};
        if (callerKnown && listsMember(*context.socketGroup, caller))
        {
            message += " although group '" + context.socketGroup->name + "' lists '" + caller
                     + "' as a member - log out and back in, or run 'newgrp " + context.socketGroup->name
                     + "' and retry from that shell";
        }
        else
        {
            message += " - join it with 'sudo usermod -aG " + context.socketGroup->name + ' '
                     + (callerKnown ? caller : std::string {"<user>"}) + "' and log in again";
        }
        return message;
    }
} // namespace mpfshost
