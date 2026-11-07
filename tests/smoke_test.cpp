#include <catch2/catch.hpp>

#include "core/version.hpp"

TEST_CASE("test binary links the unprivileged core library", "[smoke]")
{
    REQUIRE_FALSE(mpfshost::version().empty());
}
