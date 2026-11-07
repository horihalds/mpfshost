#include <catch2/catch.hpp>

#include "core/util/hex.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

TEST_CASE("hex addresses are parsed with or without a prefix", "[util][hex]")
{
    REQUIRE(mpfshost::parseHexU64("0x1000") == 0x1000);
    REQUIRE(mpfshost::parseHexU64("0XFF") == 0xFF);
    REQUIRE(mpfshost::parseHexU64("1000") == 0x1000);
    REQUIRE(mpfshost::parseHexU64("deadbeef") == 0xDEADBEEF);
    REQUIRE(mpfshost::parseHexU64("7fffffffffffffff") == 0x7FFFFFFFFFFFFFFF);
    REQUIRE(mpfshost::parseHexU64("ffffffffffffffff") == UINT64_MAX);
}

TEST_CASE("malformed hex addresses are rejected", "[util][hex]")
{
    REQUIRE_FALSE(mpfshost::parseHexU64("").has_value());
    REQUIRE_FALSE(mpfshost::parseHexU64("0x").has_value());
    REQUIRE_FALSE(mpfshost::parseHexU64("12g").has_value());
    REQUIRE_FALSE(mpfshost::parseHexU64("-1").has_value());
    REQUIRE_FALSE(mpfshost::parseHexU64("10000000000000000").has_value());
}

TEST_CASE("hex byte strings accept compact and separated forms", "[util][hex]")
{
    const std::vector<std::byte> expected {std::byte {0xDE}, std::byte {0xAD}, std::byte {0xBE}, std::byte {0xEF}};

    REQUIRE(mpfshost::parseHexBytes("deadbeef") == expected);
    REQUIRE(mpfshost::parseHexBytes("DE AD BE EF") == expected);
    REQUIRE(mpfshost::parseHexBytes("de:ad:be:ef") == expected);
    REQUIRE(mpfshost::parseHexBytes("DE,AD,BE,EF") == expected);
    REQUIRE(mpfshost::parseHexBytes("") == std::vector<std::byte> {});
}

TEST_CASE("malformed hex byte strings are rejected", "[util][hex]")
{
    REQUIRE_FALSE(mpfshost::parseHexBytes("abc").has_value());
    REQUIRE_FALSE(mpfshost::parseHexBytes("zz").has_value());
    REQUIRE_FALSE(mpfshost::parseHexBytes("de ad b").has_value());
}

TEST_CASE("hex dump renders address, byte and ascii columns", "[util][hex]")
{
    const std::vector<std::byte> data {std::byte {0x41}, std::byte {0x42}, std::byte {0x43}, std::byte {0x44}};

    const auto dump = mpfshost::formatHexDump(data, 0x1000);
    REQUIRE(dump.starts_with("0000000000001000  41 42 43 44"));
    REQUIRE(dump.find("|ABCD|") != std::string::npos);
    REQUIRE(dump.ends_with("|\n"));

    const auto lines = std::count(dump.begin(), dump.end(), '\n');
    REQUIRE(lines == 1);
}

TEST_CASE("hex dump pads a truncated tail and advances the address gutter", "[util][hex]")
{
    std::vector<std::byte> data(20);
    for (std::size_t index = 0; index < data.size(); ++index)
    {
        data[index] = static_cast<std::byte>(0x30 + static_cast<int>(index & 0x0F));
    }

    const auto dump = mpfshost::formatHexDump(data, 0x2000);
    REQUIRE(std::count(dump.begin(), dump.end(), '\n') == 2);
    REQUIRE(dump.find("0000000000002010") != std::string::npos);
    REQUIRE(dump.find("|0123456789:;<=>?|") != std::string::npos);
    REQUIRE(dump.find("|0123|") != std::string::npos);
}

TEST_CASE("hex search patterns parse bytes and wildcards", "[hex]")
{
    const auto spaced = mpfshost::parseHexPattern("de ad be ef");
    REQUIRE(spaced.has_value());
    REQUIRE(spaced->pattern.size() == 4);
    REQUIRE(spaced->pattern[0] == std::byte {0xDE});
    REQUIRE(spaced->pattern[3] == std::byte {0xEF});
    REQUIRE(spaced->mask == std::vector<std::byte>(4, std::byte {0x00}));

    const auto packed = mpfshost::parseHexPattern("deadbeef");
    REQUIRE(packed.has_value());
    REQUIRE(packed->pattern == spaced->pattern);

    const auto withPrefix = mpfshost::parseHexPattern("0xDE,0xAD");
    REQUIRE(withPrefix.has_value());
    REQUIRE(withPrefix->pattern == std::vector<std::byte>({std::byte {0xDE}, std::byte {0xAD}}));

    const auto wildcards = mpfshost::parseHexPattern("de ad ?? ef");
    REQUIRE(wildcards.has_value());
    REQUIRE(wildcards->pattern.size() == 4);
    REQUIRE(wildcards->mask[0] == std::byte {0x00});
    REQUIRE(wildcards->mask[1] == std::byte {0x00});
    REQUIRE(wildcards->mask[2] == std::byte {0xFF});
    REQUIRE(wildcards->mask[3] == std::byte {0x00});

    const auto doubleWildcard = mpfshost::parseHexPattern("aa ???? bb");
    REQUIRE(doubleWildcard.has_value());
    REQUIRE(doubleWildcard->pattern.size() == 4);
    REQUIRE(doubleWildcard->mask[1] == std::byte {0xFF});
    REQUIRE(doubleWildcard->mask[2] == std::byte {0xFF});
    REQUIRE(doubleWildcard->mask[3] == std::byte {0x00});

    REQUIRE_FALSE(mpfshost::parseHexPattern("").has_value());
    REQUIRE_FALSE(mpfshost::parseHexPattern("de a").has_value());
    REQUIRE_FALSE(mpfshost::parseHexPattern("de ? ef").has_value());
    REQUIRE_FALSE(mpfshost::parseHexPattern("de gg").has_value());
    REQUIRE_FALSE(mpfshost::parseHexPattern(std::string(33 * 2, 'a')).has_value());
}

TEST_CASE("a text search term matches every byte", "[hex]")
{
    const auto term = mpfshost::makeSearchTerm("Probe");
    REQUIRE(term.pattern.size() == 5);
    REQUIRE(term.pattern[0] == std::byte {0x50});
    REQUIRE(term.mask == std::vector<std::byte>(5, std::byte {0x00}));
}

TEST_CASE("only a term with a non-zero pattern byte is searchable", "[hex]")
{
    REQUIRE(mpfshost::isSearchableTerm(mpfshost::makeSearchTerm("Probe")));
    REQUIRE(mpfshost::isSearchableTerm(mpfshost::makeSearchTerm(std::string("\0x", 2))));

    const auto mixed = mpfshost::parseHexPattern("00 ?? 00");
    REQUIRE(mixed.has_value());
    REQUIRE_FALSE(mpfshost::isSearchableTerm(*mixed));

    // A pattern byte stays zero under a wildcard, so an all-zero term is not searchable however it is masked.
    const auto wildcards = mpfshost::parseHexPattern("?? ??");
    REQUIRE(wildcards.has_value());
    REQUIRE_FALSE(mpfshost::isSearchableTerm(*wildcards));

    const auto zeros = mpfshost::parseHexPattern("00 00");
    REQUIRE(zeros.has_value());
    REQUIRE_FALSE(mpfshost::isSearchableTerm(*zeros));

    REQUIRE_FALSE(mpfshost::isSearchableTerm(mpfshost::makeSearchTerm(std::string("\0\0", 2))));
    REQUIRE_FALSE(mpfshost::isSearchableTerm(mpfshost::SearchTerm {}));
}
