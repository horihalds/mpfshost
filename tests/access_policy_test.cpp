#include <catch2/catch.hpp>

#include "core/server/access_policy.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace
{
    mpfshost::PeerCredentials
    makeCredentials(std::uint32_t uid, std::uint32_t gid, std::vector<std::uint32_t> groups = {})
    {
        return {uid, gid, 1, std::move(groups)};
    }

    class ProcFixture
    {
    public:
        ProcFixture()
        {
            std::filesystem::create_directories(MPFSHOST_TEST_TMP_DIR);
            std::string pattern = (std::filesystem::path {MPFSHOST_TEST_TMP_DIR} / "proc-XXXXXX").string();
            REQUIRE(::mkdtemp(pattern.data()) != nullptr);
            _root = pattern;
        }

        ~ProcFixture()
        {
            std::error_code error;
            std::filesystem::remove_all(_root, error);
        }

        ProcFixture(const ProcFixture&)            = delete;
        ProcFixture& operator=(const ProcFixture&) = delete;

        [[nodiscard]] const std::filesystem::path& root() const noexcept
        {
            return _root;
        }

        void addStatus(std::int32_t pid, const std::string& content) const
        {
            const std::filesystem::path dir = _root / std::to_string(pid);
            std::filesystem::create_directories(dir);
            std::ofstream file {dir / "status"};
            REQUIRE(file);
            file << content;
        }

    private:
        std::filesystem::path _root;
    };
} // namespace

TEST_CASE("an empty access policy allows every peer", "[access]")
{
    const mpfshost::AccessPolicy policy;

    REQUIRE_FALSE(policy.enforcing());
    REQUIRE(policy.decide(std::nullopt) == mpfshost::AccessDecision::Allowed);
    REQUIRE(policy.decide(makeCredentials(1000, 1000)) == mpfshost::AccessDecision::Allowed);
    REQUIRE(policy.describe() == "open (every uid)");
}

TEST_CASE("a uid allow-list admits listed uids and refuses the rest", "[access]")
{
    mpfshost::AccessPolicy policy;
    policy.allowUid(1000);

    REQUIRE(policy.enforcing());
    REQUIRE(policy.decide(makeCredentials(1000, 1000)) == mpfshost::AccessDecision::Allowed);
    REQUIRE(policy.decide(makeCredentials(1001, 1000)) == mpfshost::AccessDecision::Denied);
}

TEST_CASE("a gid allow-list admits the peer's primary gid", "[access]")
{
    mpfshost::AccessPolicy policy;
    policy.allowGid(986);

    REQUIRE(policy.enforcing());
    REQUIRE(policy.decide(makeCredentials(1000, 986)) == mpfshost::AccessDecision::Allowed);
    REQUIRE(policy.decide(makeCredentials(1000, 987)) == mpfshost::AccessDecision::Denied);
}

TEST_CASE("a gid allow-list admits the peer's supplementary groups", "[access]")
{
    mpfshost::AccessPolicy policy;
    policy.allowGid(986, "mpfshost");

    REQUIRE(policy.decide(makeCredentials(1000, 1000, {1000, 27, 986})) == mpfshost::AccessDecision::Allowed);
    REQUIRE(policy.decide(makeCredentials(1000, 1000, {1000, 27})) == mpfshost::AccessDecision::Denied);
}

TEST_CASE("root is allowed even when the allow-list omits it", "[access]")
{
    mpfshost::AccessPolicy policy;
    policy.allowUid(1000);

    REQUIRE(policy.decide(makeCredentials(0, 0)) == mpfshost::AccessDecision::Allowed);
}

TEST_CASE("missing credentials fail closed only while the policy enforces", "[access]")
{
    mpfshost::AccessPolicy open;
    REQUIRE(open.decide(std::nullopt) == mpfshost::AccessDecision::Allowed);

    mpfshost::AccessPolicy enforcing;
    enforcing.allowUid(1000);
    REQUIRE(enforcing.decide(std::nullopt) == mpfshost::AccessDecision::CredentialsUnavailable);
}

TEST_CASE("describe lists the configured uids and gids", "[access]")
{
    mpfshost::AccessPolicy policy;
    policy.allowUid(1000);
    policy.allowGid(986);

    REQUIRE(policy.describe() == "uid 1000, gid 986");
}

TEST_CASE("describe labels a named gid", "[access]")
{
    mpfshost::AccessPolicy policy;
    policy.allowUid(1000);
    policy.allowGid(986, "mpfshost");

    REQUIRE(policy.describe() == "uid 1000, gid 986 (mpfshost)");
}

TEST_CASE("readProcessGroupIds parses the Groups line of a status file", "[access]")
{
    ProcFixture fixture;
    fixture.addStatus(4242, "Name:\ttest\nUid:\t1000\t1000\t1000\t1000\nGroups:\t1000 27 986\n");

    REQUIRE(mpfshost::readProcessGroupIds(4242, fixture.root()) == std::vector<std::uint32_t> {1000, 27, 986});
}

TEST_CASE("readProcessGroupIds tolerates a missing or empty Groups line", "[access]")
{
    ProcFixture fixture;
    fixture.addStatus(1, "Name:\ttest\nUid:\t1000\t1000\t1000\t1000\n");
    fixture.addStatus(2, "Groups:\n");

    REQUIRE(mpfshost::readProcessGroupIds(1, fixture.root()).empty());
    REQUIRE(mpfshost::readProcessGroupIds(2, fixture.root()).empty());
}

TEST_CASE("readProcessGroupIds parses the current process", "[access]")
{
    const auto groups = mpfshost::readProcessGroupIds(::getpid());
    REQUIRE(std::find(groups.begin(), groups.end(), static_cast<std::uint32_t>(::getgid())) != groups.end());
}

TEST_CASE("readProcessGroupIds returns nothing for a pid without a status file", "[access]")
{
    ProcFixture fixture;
    REQUIRE(mpfshost::readProcessGroupIds(999999, fixture.root()).empty());
}

TEST_CASE("readPeerCredentials reports the credentials of a socketpair peer", "[access]")
{
    int fds[2] {};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    const auto credentials = mpfshost::readPeerCredentials(fds[0]);
    REQUIRE(credentials.has_value());
    REQUIRE(credentials->uid == static_cast<std::uint32_t>(::getuid()));
    REQUIRE(credentials->gid == static_cast<std::uint32_t>(::getgid()));
    REQUIRE(credentials->pid == static_cast<std::int32_t>(::getpid()));
    REQUIRE(
        std::find(credentials->groupIds.begin(), credentials->groupIds.end(), static_cast<std::uint32_t>(::getgid()))
        != credentials->groupIds.end());
    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_CASE("readPeerCredentials returns nothing for an invalid descriptor", "[access]")
{
    REQUIRE_FALSE(mpfshost::readPeerCredentials(-1).has_value());
}
