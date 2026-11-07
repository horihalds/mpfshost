#include <catch2/catch.hpp>

#include "core/server/map_batching.hpp"
#include "core/server/pool_provider.hpp"
#include "core/server/pte_provider.hpp"

#include <cstdint>
#include <string>
#include <vector>

TEST_CASE("pte filter accepts every region when the request has no filter", "[core][pte]")
{
    mpfshost::PteDescriptor region;
    region.vaBase = 0x10000;
    region.pages  = 4;

    REQUIRE(mpfshost::matchesPteFilter({}, region));
}

TEST_CASE("pte filter matches the identified text case-insensitively", "[core][pte]")
{
    mpfshost::PteDescriptor region;
    region.vaBase = 0x7FF000000000;
    region.pages  = 8;
    region.text   = "C:\\Windows\\System32\\ntdll.dll";

    mpfshost::PteListRequest request;
    request.filter = "NTDLL";
    REQUIRE(mpfshost::matchesPteFilter(request, region));

    request.filter = "kernel32";
    REQUIRE_FALSE(mpfshost::matchesPteFilter(request, region));
}

TEST_CASE("pte filter applies the half-open window to the whole region span", "[core][pte]")
{
    mpfshost::PteDescriptor region;
    region.vaBase = 0x10000;
    region.pages  = 4; // the region covers [0x10000, 0x14000)

    mpfshost::PteListRequest request;
    request.minAddress = 0x12000;
    REQUIRE(mpfshost::matchesPteFilter(request, region));

    request.minAddress = 0x14000;
    REQUIRE_FALSE(mpfshost::matchesPteFilter(request, region));

    request            = {};
    request.maxAddress = 0x10000;
    REQUIRE_FALSE(mpfshost::matchesPteFilter(request, region));

    request.maxAddress = 0x10001;
    REQUIRE(mpfshost::matchesPteFilter(request, region));
}

TEST_CASE("pte filter keeps a clipped region visible for any window", "[core][pte]")
{
    mpfshost::PteDescriptor region;
    region.vaBase = 0xFFFFFFFFFFFF0000ULL;
    region.pages  = 0x100; // wraps past the end of the address space

    mpfshost::PteListRequest request;
    request.minAddress = 0x1;
    REQUIRE(mpfshost::matchesPteFilter(request, region));

    request.maxAddress = 0xFFFFFFFFFFFF0000ULL;
    REQUIRE_FALSE(mpfshost::matchesPteFilter(request, region));
}

TEST_CASE("pool filter matches the tag as a case-insensitive prefix", "[core][pool]")
{
    mpfshost::PoolDescriptor entry;
    entry.tag = static_cast<std::uint32_t>('F') | static_cast<std::uint32_t>('i') << 8
              | static_cast<std::uint32_t>('l') << 16 | static_cast<std::uint32_t>('e') << 24;

    mpfshost::PoolListRequest request;
    REQUIRE(mpfshost::matchesPoolFilter(request, entry));

    request.tagFilter = "f";
    REQUIRE(mpfshost::matchesPoolFilter(request, entry));

    request.tagFilter = "FILE";
    REQUIRE(mpfshost::matchesPoolFilter(request, entry));

    request.tagFilter = "filX";
    REQUIRE_FALSE(mpfshost::matchesPoolFilter(request, entry));

    request.tagFilter = "filex";
    REQUIRE_FALSE(mpfshost::matchesPoolFilter(request, entry));
}

TEST_CASE("pool tag filter matches the tag as a case-insensitive prefix", "[core][pool]")
{
    mpfshost::PoolTagDescriptor entry;
    entry.tag = static_cast<std::uint32_t>('F') | static_cast<std::uint32_t>('i') << 8
              | static_cast<std::uint32_t>('l') << 16 | static_cast<std::uint32_t>('e') << 24;

    mpfshost::PoolTagListRequest request;
    REQUIRE(mpfshost::matchesPoolTagFilter(request, entry));

    request.tagFilter = "f";
    REQUIRE(mpfshost::matchesPoolTagFilter(request, entry));

    request.tagFilter = "FILE";
    REQUIRE(mpfshost::matchesPoolTagFilter(request, entry));

    request.tagFilter = "filX";
    REQUIRE_FALSE(mpfshost::matchesPoolTagFilter(request, entry));

    request.tagFilter = "filex";
    REQUIRE_FALSE(mpfshost::matchesPoolTagFilter(request, entry));
}

TEST_CASE("pool filter applies the size floor and the address window", "[core][pool]")
{
    mpfshost::PoolDescriptor entry;
    entry.va    = 0xFFFF800000001000ULL;
    entry.bytes = 0x100;

    mpfshost::PoolListRequest request;
    request.minBytes = 0x100;
    REQUIRE(mpfshost::matchesPoolFilter(request, entry));

    request.minBytes = 0x101;
    REQUIRE_FALSE(mpfshost::matchesPoolFilter(request, entry));

    request            = {};
    request.minAddress = 0xFFFF800000001000ULL;
    REQUIRE(mpfshost::matchesPoolFilter(request, entry));

    request.minAddress = 0xFFFF800000001001ULL;
    REQUIRE_FALSE(mpfshost::matchesPoolFilter(request, entry));

    request            = {};
    request.maxAddress = 0xFFFF800000001000ULL;
    REQUIRE_FALSE(mpfshost::matchesPoolFilter(request, entry));

    request.maxAddress = 0xFFFF800000001001ULL;
    REQUIRE(mpfshost::matchesPoolFilter(request, entry));
}

TEST_CASE("the new encoded-size helpers match the wire layout", "[core][batching]")
{
    mpfshost::PteDescriptor region;
    region.text = "abc";
    mpfshost::PteListData pteData;
    pteData.entries.push_back(region);
    REQUIRE(mpfshost::pteEncodedSize(region) == mpfshost::encodePteListData(pteData).size() - sizeof(std::uint32_t));

    mpfshost::PoolDescriptor poolEntry;
    poolEntry.va = 0x1000;
    mpfshost::PoolListData poolData;
    poolData.entries.push_back(poolEntry);
    REQUIRE(mpfshost::poolEncodedSize(poolEntry)
            == mpfshost::encodePoolListData(poolData).size() - sizeof(std::uint32_t));

    mpfshost::PfnDescriptor pfnEntry;
    mpfshost::PfnListData   pfnData;
    pfnData.entries.push_back(pfnEntry);
    REQUIRE(mpfshost::pfnEncodedSize(pfnEntry) == mpfshost::encodePfnListData(pfnData).size() - sizeof(std::uint32_t));
}
