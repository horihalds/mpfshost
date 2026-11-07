#include <catch2/catch.hpp>

#include "core/server/handle_provider.hpp"
#include "core/server/map_batching.hpp"

#include <cstddef>
#include <string>
#include <vector>

TEST_CASE("handle filter accepts every handle when the request has no filter", "[core][handles]")
{
    mpfshost::HandleDescriptor handle;
    handle.text = "\\Device\\HarddiskVolume1";
    handle.type = "File";

    REQUIRE(mpfshost::matchesHandleFilter({4, ""}, handle));
}

TEST_CASE("handle filter matches the object name case-insensitively", "[core][handles]")
{
    mpfshost::HandleDescriptor handle;
    handle.text = "\\Device\\HarddiskVolume1";
    handle.type = "File";

    REQUIRE(mpfshost::matchesHandleFilter({4, "harddiskvolume1"}, handle));
    REQUIRE_FALSE(mpfshost::matchesHandleFilter({4, "harddiskvolume2"}, handle));
}

TEST_CASE("handle filter matches the object type", "[core][handles]")
{
    mpfshost::HandleDescriptor named;
    named.text = "\\Device\\HarddiskVolume1";
    named.type = "File";

    mpfshost::HandleDescriptor unnamed;
    unnamed.type = "Event";

    REQUIRE(mpfshost::matchesHandleFilter({4, "file"}, named));
    REQUIRE(mpfshost::matchesHandleFilter({4, "event"}, unnamed));
    REQUIRE_FALSE(mpfshost::matchesHandleFilter({4, "event"}, named));
}

TEST_CASE("handle batch budget yields multiple frames for a large result", "[core][batching]")
{
    std::vector<mpfshost::HandleDescriptor> handles(20000);
    for (auto& handle : handles)
    {
        handle.text = std::string(64, 'a');
        handle.type = std::string(16, 'b');
    }

    std::size_t batches = 0;
    for (std::size_t offset = 0; offset < handles.size();)
    {
        const auto count =
            mpfshost::batchCount(handles, offset, mpfshost::kMaxMapBatchBytes, mpfshost::handleEncodedSize);
        REQUIRE(count > 0);
        offset += count;
        ++batches;
    }
    REQUIRE(batches > 1);
}

TEST_CASE("handle encoded size matches the wire layout", "[core][batching]")
{
    mpfshost::HandleDescriptor handle;
    handle.text = "abcdef";
    handle.type = "File";

    // object, handle, access, type, two counters, two addresses, pool tag, two string prefixes
    REQUIRE(mpfshost::handleEncodedSize(handle) == 61 + handle.text.size() + handle.type.size());
}
