#include <catch2/catch.hpp>

#include "core/debug/x86_decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

namespace
{
    std::vector<std::byte> bytes(std::initializer_list<std::uint8_t> values)
    {
        std::vector<std::byte> out;
        out.reserve(values.size());
        for (const auto value : values)
        {
            out.push_back(static_cast<std::byte>(value));
        }
        return out;
    }
} // namespace

TEST_CASE("store decoder finds the instruction that filled a write watchpoint", "[core][debug][decoder]")
{
    const std::vector<std::byte> sequence = bytes({
        0x49,
        0x03,
        0xC8, // add rcx, r8
        0x44,
        0x8A,
        0x5C,
        0x11,
        0xFF, // mov r11b, [rcx+rdx-1]
        0x48,
        0xFF,
        0xC9, // dec rcx
        0x49,
        0xFF,
        0xC8, // dec r8
        0x44,
        0x88,
        0x19, // mov [rcx], r11b
    });

    const auto instruction = mpfshost::debug::decodeStoreBefore(sequence);

    REQUIRE(instruction.has_value());
    REQUIRE(instruction->offset == 14);
    REQUIRE(instruction->length == 3);
    REQUIRE(instruction->width == 1);
    REQUIRE(instruction->text == "mov byte [rcx], r11b");
}

TEST_CASE("store decoder keeps the operand width", "[core][debug][decoder]")
{
    SECTION("64-bit register store")
    {
        const auto instruction = mpfshost::debug::decodeStoreBefore(bytes({0x48, 0x89, 0x18}));
        REQUIRE(instruction.has_value());
        REQUIRE(instruction->width == 8);
        REQUIRE(instruction->text == "mov qword [rax], rbx");
    }

    SECTION("32-bit immediate store")
    {
        const auto instruction = mpfshost::debug::decodeStoreBefore(bytes({0xC7, 0x00, 0x01, 0x00, 0x00, 0x00}));
        REQUIRE(instruction.has_value());
        REQUIRE(instruction->width == 4);
        REQUIRE(instruction->text == "mov dword [rax], 0x1");
    }

    SECTION("16-bit immediate store")
    {
        const auto instruction = mpfshost::debug::decodeStoreBefore(bytes({0x66, 0xC7, 0x00, 0x01, 0x00}));
        REQUIRE(instruction.has_value());
        REQUIRE(instruction->width == 2);
        REQUIRE(instruction->text == "mov word [rax], 0x1");
    }

    SECTION("string store")
    {
        const auto instruction = mpfshost::debug::decodeStoreBefore(bytes({0xF3, 0xAB}));
        REQUIRE(instruction.has_value());
        REQUIRE(instruction->width == 4);
        REQUIRE(instruction->text == "stos dword [rdi]");
    }

    SECTION("rip-relative store")
    {
        const auto instruction = mpfshost::debug::decodeStoreBefore(bytes({0x48, 0x89, 0x05, 0x10, 0x00, 0x00, 0x00}));
        REQUIRE(instruction.has_value());
        REQUIRE(instruction->text == "mov qword [rip + 0x10], rax");
    }
}

TEST_CASE("store decoder rejects loads, comparisons and truncation", "[core][debug][decoder]")
{
    SECTION("register destination")
    {
        REQUIRE_FALSE(mpfshost::debug::decodeStoreBefore(bytes({0x48, 0x89, 0xC3})).has_value());
    }

    SECTION("load")
    {
        REQUIRE_FALSE(mpfshost::debug::decodeStoreBefore(bytes({0x8B, 0x01})).has_value());
    }

    SECTION("comparison does not write memory")
    {
        REQUIRE_FALSE(mpfshost::debug::decodeStoreBefore(bytes({0x80, 0x38, 0x01})).has_value());
    }

    SECTION("truncated instruction")
    {
        REQUIRE_FALSE(mpfshost::debug::decodeStoreBefore(bytes({0x44, 0x88})).has_value());
        REQUIRE_FALSE(mpfshost::debug::decodeStoreBefore(bytes({0x89})).has_value());
    }

    SECTION("no bytes")
    {
        REQUIRE_FALSE(mpfshost::debug::decodeStoreBefore(std::span<const std::byte> {}).has_value());
    }

    SECTION("non-store instruction")
    {
        REQUIRE_FALSE(mpfshost::debug::decodeStoreBefore(bytes({0x90})).has_value());
    }
}
