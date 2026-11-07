#include <catch2/catch.hpp>

#include "core/server/map_batching.hpp"
#include "core/server/user_provider.hpp"

#include <cstddef>
#include <string>
#include <vector>

TEST_CASE("user filter accepts everything when the request has no filter", "[core][users]")
{
    const mpfshost::UserDescriptor user {"Administrator", 0, "S-1-5-21-1"};

    REQUIRE(mpfshost::matchesUserFilter({""}, user));
}

TEST_CASE("user filter matches the name or the SID case-insensitively", "[core][users]")
{
    const mpfshost::UserDescriptor user {"Administrator", 0xFFFFA0001000ull, "S-1-5-21-1"};

    REQUIRE(mpfshost::matchesUserFilter({"admin"}, user));
    REQUIRE(mpfshost::matchesUserFilter({"ADMINISTRATOR"}, user));
    REQUIRE(mpfshost::matchesUserFilter({"s-1-5-21"}, user));
    REQUIRE_FALSE(mpfshost::matchesUserFilter({"guest"}, user));
}

TEST_CASE("user encoded size matches the wire layout", "[core][users][batching]")
{
    const mpfshost::UserDescriptor user {"Administrator", 0, "S-1-5-21-1"};

    REQUIRE(mpfshost::userEncodedSize(user) == 16 + user.name.size() + user.sid.size());
}

TEST_CASE("user batch budget yields multiple frames for a large result", "[core][users][batching]")
{
    std::vector<mpfshost::UserDescriptor> users(5000);
    for (auto& user : users)
    {
        user.name = std::string(64, 'a');
        user.sid  = std::string(64, 'b');
    }

    std::size_t batches = 0;
    for (std::size_t offset = 0; offset < users.size();)
    {
        const auto count = mpfshost::batchCount(users, offset, mpfshost::kMaxMapBatchBytes, mpfshost::userEncodedSize);
        REQUIRE(count > 0);
        offset += count;
        ++batches;
    }
    REQUIRE(batches > 1);
}
