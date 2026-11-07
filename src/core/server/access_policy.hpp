#ifndef MPFSHOST_CORE_SERVER_ACCESS_POLICY_HPP
#define MPFSHOST_CORE_SERVER_ACCESS_POLICY_HPP
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mpfshost
{
    struct PeerCredentials
    {
        std::uint32_t              uid {};
        std::uint32_t              gid {};
        std::int32_t               pid {};
        std::vector<std::uint32_t> groupIds; // Kernel group list of the peer; empty when unreadable.
    };

    enum class AccessDecision
    {
        Allowed,
        Denied,
        CredentialsUnavailable,
    };

    // Bounds how long a refused peer may stay silent before the reject thread gives up on it.
    inline constexpr std::uint32_t kAccessRejectTimeoutSeconds = 5;

    // Reads the supplementary group ids the peer process holds right now. Empty on any failure.
    [[nodiscard]] std::vector<std::uint32_t> readProcessGroupIds(std::int32_t                 pid,
                                                                 const std::filesystem::path& procRoot = "/proc");

    // Opt-in uid/gid allow-list evaluated when a connection is accepted. An empty policy is open: every peer is
    // allowed, so an unconfigured host keeps working; configuring any entry flips enforcement on.
    class AccessPolicy
    {
    public:
        void allowUid(std::uint32_t uid);
        void allowGid(std::uint32_t gid, std::string_view label = {});

        [[nodiscard]] bool           enforcing() const noexcept;
        [[nodiscard]] AccessDecision decide(const std::optional<PeerCredentials>& peer) const noexcept;
        [[nodiscard]] std::string    describe() const;

    private:
        struct GroupEntry
        {
            std::uint32_t gid {};
            std::string   label;
        };

        std::vector<std::uint32_t> _uids;
        std::vector<GroupEntry>    _gids;
    };

    [[nodiscard]] std::optional<PeerCredentials> readPeerCredentials(int fd);
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_ACCESS_POLICY_HPP
