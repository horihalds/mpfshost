#include <catch2/catch.hpp>

#include "core/util/display.hpp"

#include <string>

TEST_CASE("an absent export names array offset renders as a dash", "[display]")
{
    CHECK(mpfshost::exportNamesArrayText(mpfshost::kAbsentArrayOffset) == "-");
    CHECK(mpfshost::exportNamesArrayText(0) == "0");
    CHECK(mpfshost::exportNamesArrayText(0x1234) == "4660");
    CHECK(mpfshost::exportNamesArrayText(0xFFFFFFFEu) == "4294967294");
}

TEST_CASE("a placeholder kernel device type name renders as a single dash", "[display]")
{
    CHECK(mpfshost::kernelDeviceTypeText("") == "-");
    CHECK(mpfshost::kernelDeviceTypeText("---") == "-");
    CHECK(mpfshost::kernelDeviceTypeText("Device") == "Device");
    CHECK(mpfshost::kernelDeviceTypeText("a-b") == "a-b");
}
