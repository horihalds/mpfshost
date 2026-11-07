#include <catch2/catch.hpp>

#include "core/ipc/frame.hpp"
#include "core/server/memory_provider.hpp"
#include "core/util/page_range.hpp"

#include <cstdint>
#include <vector>

TEST_CASE("memory read ranges are validated", "[memory]")
{
    using mpfshost::isValidMemoryRange;

    REQUIRE(isValidMemoryRange(0, 1));
    REQUIRE(isValidMemoryRange(0x1000, 16));
    REQUIRE(isValidMemoryRange(0, mpfshost::kMaxReadSize));
    REQUIRE(isValidMemoryRange(UINT64_MAX - 1, 1));
}

TEST_CASE("zero, oversized and overflowing memory ranges are rejected", "[memory]")
{
    using mpfshost::isValidMemoryRange;

    REQUIRE_FALSE(isValidMemoryRange(0x1000, 0));
    REQUIRE_FALSE(isValidMemoryRange(0, mpfshost::kMaxReadSize + 1));
    REQUIRE_FALSE(isValidMemoryRange(UINT64_MAX - 1, 4));
    REQUIRE_FALSE(isValidMemoryRange(UINT64_MAX, 1));
}

TEST_CASE("page ranges expand to page-aligned addresses", "[memory]")
{
    using mpfshost::expandPageAddresses;
    using mpfshost::PageRange;

    // An unaligned start belongs to the page it fell into.
    const std::vector<PageRange> unaligned {
        {0x1008, 4}
    };
    const auto single = expandPageAddresses(unaligned, mpfshost::kMaxPageCount);
    REQUIRE(single.has_value());
    REQUIRE(single->size() == 1);
    REQUIRE(single->at(0) == 0x1000);

    // A size crossing a page boundary covers both pages.
    const std::vector<PageRange> crossing {
        {0x1fff, 2}
    };
    const auto pair = expandPageAddresses(crossing, mpfshost::kMaxPageCount);
    REQUIRE(pair.has_value());
    REQUIRE(pair->size() == 2);
    REQUIRE(pair->at(0) == 0x1000);
    REQUIRE(pair->at(1) == 0x2000);

    // Several ranges become one ascending, de-duplicated list.
    const std::vector<PageRange> several {
        {0x3000, 0x1000},
        {0x1000,      1},
        {0x3000,      1}
    };
    const auto merged = expandPageAddresses(several, mpfshost::kMaxPageCount);
    REQUIRE(merged.has_value());
    REQUIRE(merged->size() == 2);
    REQUIRE(merged->at(0) == 0x1000);
    REQUIRE(merged->at(1) == 0x3000);

    // An exactly page-sized range yields exactly one address.
    const std::vector<PageRange> exact {
        {0x5000, mpfshost::kPageSize}
    };
    const auto one = expandPageAddresses(exact, mpfshost::kMaxPageCount);
    REQUIRE(one.has_value());
    REQUIRE(one->size() == 1);
}

TEST_CASE("empty, inverted and oversized page ranges are rejected", "[memory]")
{
    using mpfshost::expandPageAddresses;
    using mpfshost::PageRange;

    REQUIRE_FALSE(expandPageAddresses({}, mpfshost::kMaxPageCount).has_value());

    const std::vector<PageRange> emptySize {
        {0x1000, 0}
    };
    REQUIRE_FALSE(expandPageAddresses(emptySize, mpfshost::kMaxPageCount).has_value());

    const std::vector<PageRange> overflowing {
        {UINT64_MAX - 1, 4}
    };
    REQUIRE_FALSE(expandPageAddresses(overflowing, mpfshost::kMaxPageCount).has_value());

    const std::vector<PageRange> tooMany {
        {0, (mpfshost::kMaxPageCount + 1) * mpfshost::kPageSize}
    };
    REQUIRE_FALSE(expandPageAddresses(tooMany, mpfshost::kMaxPageCount).has_value());

    // The cap also applies across ranges, not only inside one.
    const std::vector<PageRange> sumTooLarge {
        {      0,     mpfshost::kPageSize},
        {0x10000, 2 * mpfshost::kPageSize}
    };
    REQUIRE_FALSE(expandPageAddresses(sumTooLarge, 2).has_value());
}
