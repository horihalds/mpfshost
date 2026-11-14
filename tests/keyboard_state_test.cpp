#include <catch2/catch.hpp>

#include "core/util/keyboard_state.hpp"
#include "core/util/signature.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
    std::vector<std::byte> bytes(std::initializer_list<std::uint8_t> values)
    {
        std::vector<std::byte> result;
        result.reserve(values.size());
        for (const auto value : values)
        {
            result.push_back(static_cast<std::byte>(value));
        }
        return result;
    }
} // namespace

TEST_CASE("signature patterns match exact and wildcard bytes", "[util][signature]")
{
    const auto pattern = mpfshost::parseSignature("48 8B 05 ? ? ? ? C3");
    REQUIRE(pattern.has_value());

    auto       data  = bytes({0x48, 0x8B, 0x05, 0x11, 0x22, 0x33, 0x44, 0xC3});
    const auto match = mpfshost::findSignature(data, *pattern);
    REQUIRE(match.has_value());
    REQUIRE(match->offset == 0);

    data[4] = std::byte {0};
    REQUIRE(mpfshost::findSignature(data, *pattern).has_value());

    data[0] = std::byte {0};
    REQUIRE_FALSE(mpfshost::findSignature(data, *pattern).has_value());
}

TEST_CASE("signature patterns capture a little-endian u4", "[util][signature]")
{
    const auto pattern = mpfshost::parseSignature("B9 00 80 FF FF ? 22 B4 ? u4 41");
    REQUIRE(pattern.has_value());

    const auto data  = bytes({0xB9, 0x00, 0x80, 0xFF, 0xFF, 0x00, 0x22, 0xB4, 0x00, 0x08, 0x38, 0x00, 0x00, 0x41});
    const auto match = mpfshost::findSignature(data, *pattern);
    REQUIRE(match.has_value());
    REQUIRE(match->offset == 0);
    REQUIRE(match->captured == 0x3808);
}

TEST_CASE("signature patterns reject malformed text", "[util][signature]")
{
    REQUIRE_FALSE(mpfshost::parseSignature("").has_value());
    REQUIRE_FALSE(mpfshost::parseSignature("4").has_value());
    REQUIRE_FALSE(mpfshost::parseSignature("4G").has_value());
    REQUIRE_FALSE(mpfshost::parseSignature("u4 u4").has_value());
    REQUIRE_FALSE(mpfshost::parseSignature("   ").has_value());
}

TEST_CASE("signature matching stops at the first hit", "[util][signature]")
{
    const auto pattern = mpfshost::parseSignature("AA BB");
    REQUIRE(pattern.has_value());

    const auto data  = bytes({0x00, 0xAA, 0xBB, 0xAA, 0xBB});
    const auto match = mpfshost::findSignature(data, *pattern);
    REQUIRE(match.has_value());
    REQUIRE(match->offset == 1);
}

TEST_CASE("keyboard state decodes two bits per virtual key", "[util][keyboard]")
{
    std::vector<std::byte> state(mpfshost::kKeyboardStateBytes, std::byte {0});
    state[0x10 / 4] = static_cast<std::byte>(1u << ((0x10 % 4) * 2));
    state[0x41 / 4] = static_cast<std::byte>(1u << ((0x41 % 4) * 2));

    const auto pressed = mpfshost::decodeKeyboardState(state);
    REQUIRE(pressed.size() == 2);
    REQUIRE(pressed[0] == 0x10);
    REQUIRE(pressed[1] == 0x41);

    REQUIRE(mpfshost::decodeKeyboardState(std::span<const std::byte> {}).empty());
}

TEST_CASE("virtual key names cover the common keys", "[util][keyboard]")
{
    REQUIRE(mpfshost::virtualKeyName(0x10) == "shift");
    REQUIRE(mpfshost::virtualKeyName(0x0D) == "return");
    REQUIRE(mpfshost::virtualKeyName(0x41) == "a");
    REQUIRE(mpfshost::virtualKeyName(0x70) == "f1");
    REQUIRE(mpfshost::virtualKeyName(0xFF).empty());
}
