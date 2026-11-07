#include <catch2/catch.hpp>

#include "core/ipc/frame.hpp"
#include "core/ipc/message.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

TEST_CASE("pte list request round trips", "[core][message][pte]")
{
    mpfshost::PteListRequest request;
    request.pid             = 4321;
    request.identifyModules = 1;
    request.minAddress      = 0x10000;
    request.maxAddress      = 0x7FFFFFFF0000ULL;
    request.filter          = "ntdll";

    const auto encoded = mpfshost::encodePteListRequest(request);
    const auto decoded = mpfshost::decodePteListRequest(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->pid == request.pid);
    REQUIRE(decoded->identifyModules == 1);
    REQUIRE(decoded->minAddress == request.minAddress);
    REQUIRE(decoded->maxAddress == request.maxAddress);
    REQUIRE(decoded->filter == request.filter);
}

TEST_CASE("pte list data round trips every field", "[core][message][pte]")
{
    mpfshost::PteDescriptor region;
    region.vaBase        = 0x7FF000000000;
    region.pages         = 0x40;
    region.pageFlags     = 0x8000000000000000ULL;
    region.softwarePages = 3;
    region.wow64         = 1;
    region.text          = "ntdll.dll";

    mpfshost::PteListData data;
    data.entries.push_back(region);

    const auto decoded = mpfshost::decodePteListData(mpfshost::encodePteListData(data));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->entries.size() == 1);
    REQUIRE(decoded->entries[0].vaBase == region.vaBase);
    REQUIRE(decoded->entries[0].pages == region.pages);
    REQUIRE(decoded->entries[0].pageFlags == region.pageFlags);
    REQUIRE(decoded->entries[0].softwarePages == region.softwarePages);
    REQUIRE(decoded->entries[0].wow64 == 1);
    REQUIRE(decoded->entries[0].text == region.text);
}

TEST_CASE("pte list end carries the totals and the truncation flag", "[core][message][pte]")
{
    mpfshost::PteListEnd end;
    end.status     = 0;
    end.entryCount = 12;
    end.mapEntries = 40;
    end.truncated  = 1;
    end.error      = "";

    const auto decoded = mpfshost::decodePteListEnd(mpfshost::encodePteListEnd(end));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->entryCount == 12);
    REQUIRE(decoded->mapEntries == 40);
    REQUIRE(decoded->truncated == 1);
}

TEST_CASE("a truncated pte entry list is rejected", "[core][message][pte]")
{
    mpfshost::PteListData data;
    data.entries.resize(2);
    auto encoded = mpfshost::encodePteListData(data);
    encoded.resize(encoded.size() - 4);

    REQUIRE_FALSE(mpfshost::decodePteListData(encoded).has_value());
}

TEST_CASE("a pte entry count above the payload size is rejected", "[core][message][pte]")
{
    std::vector<std::byte> payload;
    payload.push_back(std::byte {0xFF});
    payload.push_back(std::byte {0xFF});
    payload.push_back(std::byte {0xFF});
    payload.push_back(std::byte {0xFF});
    REQUIRE_FALSE(mpfshost::decodePteListData(payload).has_value());
}

TEST_CASE("pool list request round trips", "[core][message][pool]")
{
    mpfshost::PoolListRequest request;
    request.big        = 1;
    request.tagFilter  = "Fi";
    request.minBytes   = 0x80;
    request.minAddress = 0xFFFF800000000000ULL;
    request.maxAddress = 0xFFFFC00000000000ULL;

    const auto decoded = mpfshost::decodePoolListRequest(mpfshost::encodePoolListRequest(request));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->big == 1);
    REQUIRE(decoded->tagFilter == request.tagFilter);
    REQUIRE(decoded->minBytes == request.minBytes);
    REQUIRE(decoded->minAddress == request.minAddress);
    REQUIRE(decoded->maxAddress == request.maxAddress);
}

TEST_CASE("pool list data round trips every field", "[core][message][pool]")
{
    mpfshost::PoolDescriptor entry;
    entry.va         = 0xFFFF800000001000ULL;
    entry.bytes      = 0x1000;
    entry.tag        = 0x656C6946;
    entry.alloc      = 1;
    entry.poolType   = 2;
    entry.subsegment = 5;

    mpfshost::PoolListData data;
    data.entries.push_back(entry);

    const auto decoded = mpfshost::decodePoolListData(mpfshost::encodePoolListData(data));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->entries.size() == 1);
    REQUIRE(decoded->entries[0].va == entry.va);
    REQUIRE(decoded->entries[0].bytes == entry.bytes);
    REQUIRE(decoded->entries[0].tag == entry.tag);
    REQUIRE(decoded->entries[0].alloc == 1);
    REQUIRE(decoded->entries[0].poolType == entry.poolType);
    REQUIRE(decoded->entries[0].subsegment == entry.subsegment);
}

TEST_CASE("pool list end carries the map size and the truncation flag", "[core][message][pool]")
{
    mpfshost::PoolListEnd end;
    end.entryCount = 5;
    end.mapEntries = 9;
    end.mapBytes   = 0x123456;
    end.truncated  = 1;

    const auto decoded = mpfshost::decodePoolListEnd(mpfshost::encodePoolListEnd(end));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->entryCount == 5);
    REQUIRE(decoded->mapEntries == 9);
    REQUIRE(decoded->mapBytes == 0x123456);
    REQUIRE(decoded->truncated == 1);
}

TEST_CASE("a truncated pool entry list is rejected", "[core][message][pool]")
{
    mpfshost::PoolListData data;
    data.entries.resize(1);
    auto encoded = mpfshost::encodePoolListData(data);
    encoded.resize(encoded.size() - 1);

    REQUIRE_FALSE(mpfshost::decodePoolListData(encoded).has_value());
}

TEST_CASE("pool tag list request round trips", "[core][message][pool]")
{
    mpfshost::PoolTagListRequest request;
    request.big       = 1;
    request.tagFilter = "Fi";

    const auto decoded = mpfshost::decodePoolTagListRequest(mpfshost::encodePoolTagListRequest(request));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->big == 1);
    REQUIRE(decoded->tagFilter == request.tagFilter);
}

TEST_CASE("pool tag list data round trips every field", "[core][message][pool]")
{
    mpfshost::PoolTagDescriptor entry;
    entry.tag     = 0x656C6946;
    entry.entries = 0x1234;

    mpfshost::PoolTagListData data;
    data.tags.push_back(entry);

    const auto decoded = mpfshost::decodePoolTagListData(mpfshost::encodePoolTagListData(data));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->tags.size() == 1);
    REQUIRE(decoded->tags[0].tag == entry.tag);
    REQUIRE(decoded->tags[0].entries == entry.entries);
}

TEST_CASE("pool tag list end carries the totals and the truncation flag", "[core][message][pool]")
{
    mpfshost::PoolTagListEnd end;
    end.entryCount = 5;
    end.mapTags    = 9;
    end.mapEntries = 0x1234;
    end.truncated  = 1;

    const auto decoded = mpfshost::decodePoolTagListEnd(mpfshost::encodePoolTagListEnd(end));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->entryCount == 5);
    REQUIRE(decoded->mapTags == 9);
    REQUIRE(decoded->mapEntries == 0x1234);
    REQUIRE(decoded->truncated == 1);
}

TEST_CASE("a truncated pool tag list is rejected", "[core][message][pool]")
{
    mpfshost::PoolTagListData data;
    data.tags.resize(1);
    auto encoded = mpfshost::encodePoolTagListData(data);
    encoded.resize(encoded.size() - 1);

    REQUIRE_FALSE(mpfshost::decodePoolTagListData(encoded).has_value());
}

TEST_CASE("pfn list request round trips", "[core][message][pfn]")
{
    const mpfshost::PfnListRequest request {0x1F000, 64};
    const auto                     decoded = mpfshost::decodePfnListRequest(mpfshost::encodePfnListRequest(request));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->pfnStart == request.pfnStart);
    REQUIRE(decoded->pfnCount == request.pfnCount);
}

TEST_CASE("pfn list request honours the entry cap", "[core][message][pfn]")
{
    REQUIRE(mpfshost::kMaxPfnEntries == 0x8000);

    mpfshost::PfnListRequest request;
    request.pfnStart = 0;
    request.pfnCount = static_cast<std::uint32_t>(mpfshost::kMaxPfnEntries);

    const auto decoded = mpfshost::decodePfnListRequest(mpfshost::encodePfnListRequest(request));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->pfnCount == mpfshost::kMaxPfnEntries);
}

TEST_CASE("pfn list data round trips every field", "[core][message][pfn]")
{
    mpfshost::PfnDescriptor entry;
    entry.pfn            = 0x1234;
    entry.typeExtended   = 3;
    entry.pageLocation   = 6;
    entry.pid            = 4242;
    entry.va             = 0x7FF000001000;
    entry.vaPte          = 0xFFFFF68000100000ULL;
    entry.originalPte    = 0x1;
    entry.pteFrame       = 0xABCD;
    entry.referenceCount = 7;
    entry.cacheAttribute = 4;
    entry.priority       = 2;
    entry.flags          = 0x41;
    entry.pageColor      = 9;

    mpfshost::PfnListData data;
    data.entries.push_back(entry);

    const auto decoded = mpfshost::decodePfnListData(mpfshost::encodePfnListData(data));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->entries.size() == 1);
    const auto& actual = decoded->entries[0];
    REQUIRE(actual.pfn == entry.pfn);
    REQUIRE(actual.typeExtended == entry.typeExtended);
    REQUIRE(actual.pageLocation == entry.pageLocation);
    REQUIRE(actual.pid == entry.pid);
    REQUIRE(actual.va == entry.va);
    REQUIRE(actual.vaPte == entry.vaPte);
    REQUIRE(actual.originalPte == entry.originalPte);
    REQUIRE(actual.pteFrame == entry.pteFrame);
    REQUIRE(actual.referenceCount == entry.referenceCount);
    REQUIRE(actual.cacheAttribute == entry.cacheAttribute);
    REQUIRE(actual.priority == entry.priority);
    REQUIRE(actual.flags == entry.flags);
    REQUIRE(actual.pageColor == entry.pageColor);
}

TEST_CASE("a truncated pfn entry list is rejected", "[core][message][pfn]")
{
    mpfshost::PfnListData data;
    data.entries.resize(3);
    auto encoded = mpfshost::encodePfnListData(data);
    encoded.resize(encoded.size() - 2);

    REQUIRE_FALSE(mpfshost::decodePfnListData(encoded).has_value());
}

TEST_CASE("the new message types are named in both directions", "[core][message]")
{
    REQUIRE(mpfshost::messageTypeName(mpfshost::MessageType::PteListRequest) == "PteListRequest");
    REQUIRE(mpfshost::messageTypeName(mpfshost::MessageType::PoolListRequest) == "PoolListRequest");
    REQUIRE(mpfshost::messageTypeName(mpfshost::MessageType::PfnListRequest) == "PfnListRequest");
    REQUIRE(mpfshost::messageTypeName(mpfshost::MessageType::PteListData) == "PteListData");
    REQUIRE(mpfshost::messageTypeName(mpfshost::MessageType::PoolListEnd) == "PoolListEnd");
    REQUIRE(mpfshost::messageTypeName(mpfshost::MessageType::PfnListEnd) == "PfnListEnd");
}
