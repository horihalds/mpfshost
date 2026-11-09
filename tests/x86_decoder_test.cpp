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

TEST_CASE("disassembler renders lengths and Intel text", "[core][debug][decoder]")
{
    SECTION("a register move")
    {
        const auto instructions = mpfshost::debug::disassemble(bytes({0x48, 0x89, 0xD8}), 0x1000, 8);
        REQUIRE(instructions.size() == 1);
        REQUIRE(instructions[0].address == 0x1000);
        REQUIRE(instructions[0].bytes.size() == 3);
        REQUIRE(instructions[0].text.find("mov") != std::string::npos);
        REQUIRE(instructions[0].text.find("rax") != std::string::npos);
        REQUIRE(instructions[0].text.find("rbx") != std::string::npos);
    }

    SECTION("a prefix is part of the instruction")
    {
        const auto instructions = mpfshost::debug::disassemble(bytes({0xF3, 0x90}), 0x2000, 8);
        REQUIRE(instructions.size() == 1);
        REQUIRE(instructions[0].address == 0x2000);
        REQUIRE(instructions[0].bytes.size() == 2);
        REQUIRE(instructions[0].text == "pause");
    }

    SECTION("several instructions advance the address")
    {
        const auto instructions = mpfshost::debug::disassemble(bytes({0x90, 0x48, 0x89, 0xD8}), 0x3000, 8);
        REQUIRE(instructions.size() == 2);
        REQUIRE(instructions[0].text == "nop");
        REQUIRE(instructions[1].address == 0x3001);
        REQUIRE(instructions[1].bytes.size() == 3);
    }
}

TEST_CASE("disassembler emits unknown bytes instead of guessing", "[core][debug][decoder]")
{
    const auto instructions = mpfshost::debug::disassemble(bytes({0x06, 0x90}), 0x4000, 8);
    REQUIRE(instructions.size() == 2);
    REQUIRE(instructions[0].address == 0x4000);
    REQUIRE(instructions[0].bytes.size() == 1);
    REQUIRE(instructions[0].text == ".byte 0x06");
    REQUIRE(instructions[1].address == 0x4001);
    REQUIRE(instructions[1].text == "nop");
}

TEST_CASE("disassembler truncates at the requested instruction count", "[core][debug][decoder]")
{
    const auto instructions = mpfshost::debug::disassemble(bytes({0x90, 0x90, 0x90, 0x90}), 0x5000, 2);
    REQUIRE(instructions.size() == 2);
    REQUIRE(instructions[0].address == 0x5000);
    REQUIRE(instructions[1].address == 0x5001);

    REQUIRE(mpfshost::debug::disassemble(bytes({0x90}), 0x5000, 0).empty());
    REQUIRE(mpfshost::debug::disassemble(std::span<const std::byte> {}, 0x5000, 4).empty());
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
