#include <catch2/catch.hpp>

#include "core/server/process_provider.hpp"

TEST_CASE("process filter accepts everything when both filters are empty", "[core][process]")
{
    const mpfshost::ProcessFilter     filter;
    const mpfshost::ProcessDescriptor process {1234, 4, "explorer.exe", false};

    REQUIRE(mpfshost::matchesProcessFilter(filter, process));
}

TEST_CASE("process filter matches a non-zero pid exactly", "[core][process]")
{
    const mpfshost::ProcessDescriptor process {1234, 4, "explorer.exe", false};

    REQUIRE(mpfshost::matchesProcessFilter({1234, ""}, process));
    REQUIRE_FALSE(mpfshost::matchesProcessFilter({4321, ""}, process));
}

TEST_CASE("process filter matches a name substring case-insensitively", "[core][process]")
{
    const mpfshost::ProcessDescriptor process {1234, 4, "Explorer.EXE", false};

    REQUIRE(mpfshost::matchesProcessFilter({0, "explorer"}, process));
    REQUIRE(mpfshost::matchesProcessFilter({0, "PLORE"}, process));
    REQUIRE(mpfshost::matchesProcessFilter({0, ".exe"}, process));
    REQUIRE_FALSE(mpfshost::matchesProcessFilter({0, "notepad"}, process));
}

TEST_CASE("process filter combines the pid and name filters", "[core][process]")
{
    const mpfshost::ProcessDescriptor process {1234, 4, "explorer.exe", false};

    REQUIRE(mpfshost::matchesProcessFilter({1234, "explore"}, process));
    REQUIRE_FALSE(mpfshost::matchesProcessFilter({4321, "explore"}, process));
    REQUIRE_FALSE(mpfshost::matchesProcessFilter({1234, "notepad"}, process));
}
