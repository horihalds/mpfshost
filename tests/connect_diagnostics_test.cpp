#include <catch2/catch.hpp>

#include "core/client/connect_diagnostics.hpp"
#include "core/util/identity.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace
{
    class SocketFixture
    {
    public:
        SocketFixture()
        {
            std::filesystem::create_directories(MPFSHOST_TEST_TMP_DIR);
            std::string pattern = (std::filesystem::path {MPFSHOST_TEST_TMP_DIR} / "connect-XXXXXX").string();
            REQUIRE(::mkdtemp(pattern.data()) != nullptr);
            _root       = pattern;
            _socketPath = _root / "host.sock";
            REQUIRE(_socketPath.string().size() < sizeof(sockaddr_un::sun_path));

            const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            REQUIRE(fd >= 0);

            sockaddr_un address {};
            address.sun_family = AF_UNIX;
            const auto text    = _socketPath.string();
            std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
            REQUIRE(::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
            REQUIRE(::chmod(_socketPath.c_str(), 0660) == 0);
            ::close(fd);
        }

        ~SocketFixture()
        {
            std::error_code error;
            std::filesystem::remove_all(_root, error);
        }

        SocketFixture(const SocketFixture&)            = delete;
        SocketFixture& operator=(const SocketFixture&) = delete;

        [[nodiscard]] const std::filesystem::path& root() const noexcept
        {
            return _root;
        }

        [[nodiscard]] const std::filesystem::path& socketPath() const noexcept
        {
            return _socketPath;
        }

    private:
        std::filesystem::path _root;
        std::filesystem::path _socketPath;
    };

    mpfshost::ConnectDenialContext staleMembershipContext()
    {
        mpfshost::ConnectDenialContext context;
        context.socketPath     = "/run/mpfshost.sock";
        context.errorText      = "Permission denied";
        context.file           = mpfshost::SocketFileInfo {0660, 0, 964};
        context.callerUid      = 1000;
        context.callerName     = "user";
        context.callerGroupIds = {1000, 10, 36};

        mpfshost::GroupRecord group;
        group.name          = "mpfshost";
        group.gid           = 964;
        group.members       = {"user"};
        context.socketGroup = group;
        return context;
    }
} // namespace

TEST_CASE("socket file inspection reports mode owner and group", "[smoke]")
{
    SocketFixture fixture;

    const auto info = mpfshost::readSocketFileInfo(fixture.socketPath());
    REQUIRE(info.has_value());
    CHECK(info->mode == 0660U);
    CHECK(info->ownerUid == static_cast<std::uint32_t>(::geteuid()));
    CHECK(info->groupGid == static_cast<std::uint32_t>(::getegid()));

    CHECK_FALSE(mpfshost::readSocketFileInfo(fixture.root() / "missing.sock").has_value());
}

TEST_CASE("current group ids contain the real and effective gid", "[smoke]")
{
    const auto ids = mpfshost::currentGroupIds();
    REQUIRE_FALSE(ids.empty());
    CHECK(std::find(ids.begin(), ids.end(), static_cast<std::uint32_t>(::getegid())) != ids.end());
    CHECK(std::find(ids.begin(), ids.end(), static_cast<std::uint32_t>(::getgid())) != ids.end());

    const std::set<std::uint32_t> unique(ids.begin(), ids.end());
    CHECK(unique.size() == ids.size());
}

TEST_CASE("group and user lookups resolve known identities", "[smoke]")
{
    const auto record = mpfshost::readGroupInfo(static_cast<std::uint32_t>(::getegid()));
    REQUIRE(record.has_value());
    CHECK(record->gid == static_cast<std::uint32_t>(::getegid()));
    CHECK_FALSE(record->name.empty());

    CHECK_FALSE(mpfshost::readGroupInfo(4000000000U).has_value());

    const auto name = mpfshost::readUserName(0);
    REQUIRE(name.has_value());
    CHECK_FALSE(name->empty());
}

TEST_CASE("connect denial explains a stale group membership", "[smoke]")
{
    const auto text = mpfshost::describeConnectDenial(staleMembershipContext());

    CHECK(text.find("connect failed: Permission denied") != std::string::npos);
    CHECK(text.find("/run/mpfshost.sock") != std::string::npos);
    CHECK(text.find("mode 0660") != std::string::npos);
    CHECK(text.find("group 'mpfshost' gid 964") != std::string::npos);
    CHECK(text.find("[1000, 10, 36]") != std::string::npos);
    CHECK(text.find("lists 'user' as a member") != std::string::npos);
    CHECK(text.find("newgrp mpfshost") != std::string::npos);
    CHECK(text.find('\n') == std::string::npos);
}

TEST_CASE("connect denial suggests joining the group when the caller is not listed", "[smoke]")
{
    auto context                 = staleMembershipContext();
    context.socketGroup->members = {"someone-else"};

    const auto text = mpfshost::describeConnectDenial(context);
    CHECK(text.find("sudo usermod -aG mpfshost user") != std::string::npos);
    CHECK(text.find("newgrp") == std::string::npos);
}

TEST_CASE("connect denial avoids a numeric uid when the caller name is unknown", "[smoke]")
{
    auto context                 = staleMembershipContext();
    context.callerName           = std::nullopt;
    context.socketGroup->members = {};

    const auto text = mpfshost::describeConnectDenial(context);
    CHECK(text.find("sudo usermod -aG mpfshost <user>") != std::string::npos);
}

TEST_CASE("connect denial points at directory permissions when the gid is already held", "[smoke]")
{
    auto context           = staleMembershipContext();
    context.callerGroupIds = {1000, 10, 964};

    const auto text = mpfshost::describeConnectDenial(context);
    CHECK(text.find("already holds gid 964") != std::string::npos);
    CHECK(text.find("permissions of the socket's directory") != std::string::npos);
    CHECK(text.find("newgrp") == std::string::npos);
    CHECK(text.find("usermod") == std::string::npos);
}

TEST_CASE("connect denial degrades gracefully without file or group data", "[smoke]")
{
    mpfshost::ConnectDenialContext context;
    context.socketPath = "/tmp/missing.sock";
    context.errorText  = "Permission denied";

    const auto missing = mpfshost::describeConnectDenial(context);
    CHECK(missing.find("connect failed: Permission denied") != std::string::npos);
    CHECK(missing.find("/tmp/missing.sock") != std::string::npos);
    CHECK(missing.find("gid") == std::string::npos);
    CHECK(missing.find('\n') == std::string::npos);
}

TEST_CASE("connect denial falls back to the numeric gid when the group is unknown", "[smoke]")
{
    auto context           = staleMembershipContext();
    context.socketGroup    = std::nullopt;
    context.callerGroupIds = {1000, 10};

    const auto text = mpfshost::describeConnectDenial(context);
    CHECK(text.find("group gid 964") != std::string::npos);
    CHECK(text.find("do not include 964") != std::string::npos);
    CHECK(text.find("newgrp") == std::string::npos);
    CHECK(text.find("usermod") == std::string::npos);
}
