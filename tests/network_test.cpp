#include <catch2/catch.hpp>

#include "core/server/map_batching.hpp"
#include "core/server/network_provider.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace
{
    mpfshost::NetworkEntryDescriptor makeEntry(std::uint32_t pid, std::string process)
    {
        mpfshost::NetworkEntryDescriptor entry;
        entry.pid           = pid;
        entry.process       = std::move(process);
        entry.addressFamily = 2;
        return entry;
    }
} // namespace

TEST_CASE("network filter accepts every connection when the request is unscoped", "[core][net]")
{
    mpfshost::NetworkListRequest request;

    REQUIRE(mpfshost::matchesNetworkFilter(request, makeEntry(4321, "demo.exe")));
    REQUIRE(mpfshost::matchesNetworkFilter(request, makeEntry(777, "other.exe")));
}

TEST_CASE("network filter narrows the answer to the requested pid", "[core][net]")
{
    mpfshost::NetworkListRequest request;
    request.pid = 4321;

    REQUIRE(mpfshost::matchesNetworkFilter(request, makeEntry(4321, "demo.exe")));
    REQUIRE_FALSE(mpfshost::matchesNetworkFilter(request, makeEntry(777, "demo.exe")));
}

TEST_CASE("network filter matches the process name and both endpoints", "[core][net]")
{
    mpfshost::NetworkEntryDescriptor entry = makeEntry(4321, "demo.exe");
    entry.local.address                    = "10.0.0.5:445";
    entry.remote.address                   = "10.0.0.9:50000";

    REQUIRE(mpfshost::matchesNetworkFilter({0, "demo"}, entry));
    REQUIRE(mpfshost::matchesNetworkFilter({0, "DEMO.EXE"}, entry));
    REQUIRE(mpfshost::matchesNetworkFilter({0, "10.0.0.5"}, entry));
    REQUIRE(mpfshost::matchesNetworkFilter({0, "50000"}, entry));
    REQUIRE_FALSE(mpfshost::matchesNetworkFilter({0, "127.0.0.1"}, entry));
}

TEST_CASE("network filter applies the pid scope together with the text filter", "[core][net]")
{
    mpfshost::NetworkEntryDescriptor entry = makeEntry(4321, "demo.exe");
    entry.local.address                    = "10.0.0.5:445";

    REQUIRE(mpfshost::matchesNetworkFilter({4321, "445"}, entry));
    REQUIRE_FALSE(mpfshost::matchesNetworkFilter({777, "445"}, entry));
}

TEST_CASE("network entry encoded size matches the wire layout", "[core][net][batching]")
{
    mpfshost::NetworkEntryDescriptor entry = makeEntry(4321, "abc");
    entry.local.address                    = "de";
    entry.remote.address                   = "fgh";

    const auto strings = entry.process.size() + entry.local.address.size() + entry.remote.address.size();
    REQUIRE(mpfshost::networkEntryEncodedSize(entry) == 44 + strings);
}

TEST_CASE("network batch budget yields multiple frames for a large result", "[core][net][batching]")
{
    std::vector<mpfshost::NetworkEntryDescriptor> entries(10000);
    for (auto& entry : entries)
    {
        entry               = makeEntry(4321, "demo.exe");
        entry.local.address = std::string(32, 'a');
    }

    std::size_t batches = 0;
    for (std::size_t offset = 0; offset < entries.size();)
    {
        const auto count =
            mpfshost::batchCount(entries, offset, mpfshost::kMaxMapBatchBytes, mpfshost::networkEntryEncodedSize);
        REQUIRE(count > 0);
        offset += count;
        ++batches;
    }
    REQUIRE(batches > 1);
}
