#include "core/server/access_policy.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include <sys/socket.h>

namespace mpfshost
{
    namespace
    {
        std::vector<std::uint32_t> parseGroupIds(std::string_view text)
        {
            std::vector<std::uint32_t> ids;
            std::size_t                pos = 0;
            while (pos < text.size())
            {
                while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t'))
                {
                    ++pos;
                }
                const std::size_t begin = pos;
                while (pos < text.size() && text[pos] != ' ' && text[pos] != '\t')
                {
                    ++pos;
                }
                if (begin == pos)
                {
                    break;
                }
                std::uint32_t gid  = 0;
                const auto    read = std::from_chars(text.data() + begin, text.data() + pos, gid);
                if (read.ec == std::errc {} && read.ptr == text.data() + pos)
                {
                    ids.push_back(gid);
                }
            }
            return ids;
        }
    } // namespace

    void AccessPolicy::allowUid(std::uint32_t uid)
    {
        _uids.push_back(uid);
    }

    void AccessPolicy::allowGid(std::uint32_t gid, std::string_view label)
    {
        _gids.push_back(GroupEntry {gid, std::string {label}});
    }

    bool AccessPolicy::enforcing() const noexcept
    {
        return !_uids.empty() || !_gids.empty();
    }

    AccessDecision AccessPolicy::decide(const std::optional<PeerCredentials>& peer) const noexcept
    {
        if (!enforcing())
        {
            return AccessDecision::Allowed;
        }
        if (!peer)
        {
            return AccessDecision::CredentialsUnavailable;
        }
        if (peer->uid == 0)
        {
            return AccessDecision::Allowed;
        }
        if (std::find(_uids.begin(), _uids.end(), peer->uid) != _uids.end())
        {
            return AccessDecision::Allowed;
        }
        for (const auto& entry : _gids)
        {
            if (peer->gid == entry.gid
                || std::find(peer->groupIds.begin(), peer->groupIds.end(), entry.gid) != peer->groupIds.end())
            {
                return AccessDecision::Allowed;
            }
        }
        return AccessDecision::Denied;
    }

    std::string AccessPolicy::describe() const
    {
        if (!enforcing())
        {
            return "open (every uid)";
        }

        std::string text;
        const auto  append = [&text](std::string_view prefix, std::uint32_t id)
        {
            if (!text.empty())
            {
                text += ", ";
            }
            text += prefix;
            text += std::to_string(id);
        };
        for (const auto uid : _uids)
        {
            append("uid ", uid);
        }
        for (const auto& entry : _gids)
        {
            append("gid ", entry.gid);
            if (!entry.label.empty())
            {
                text += " (";
                text += entry.label;
                text += ")";
            }
        }
        return text;
    }

    std::vector<std::uint32_t> readProcessGroupIds(std::int32_t pid, const std::filesystem::path& procRoot)
    {
        std::ifstream status {procRoot / std::to_string(pid) / "status"};
        if (!status)
        {
            return {};
        }

        constexpr std::string_view prefix = "Groups:";
        std::string                line;
        while (std::getline(status, line))
        {
            if (std::string_view {line}.substr(0, prefix.size()) == prefix)
            {
                return parseGroupIds(std::string_view {line}.substr(prefix.size()));
            }
        }
        return {};
    }

    std::optional<PeerCredentials> readPeerCredentials(int fd)
    {
        ucred     credentials {};
        socklen_t length = sizeof(credentials);
        if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0)
        {
            return std::nullopt;
        }
        PeerCredentials peer {static_cast<std::uint32_t>(credentials.uid),
                              static_cast<std::uint32_t>(credentials.gid),
                              static_cast<std::int32_t>(credentials.pid),
                              {}};
        // A failed group read leaves the list empty and can only narrow access, so it never invalidates the peer.
        peer.groupIds = readProcessGroupIds(peer.pid);
        return peer;
    }
} // namespace mpfshost
