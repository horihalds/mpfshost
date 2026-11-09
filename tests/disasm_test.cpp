#include <catch2/catch.hpp>

#include "core/disasm/disassembler.hpp"
#include "core/disasm/x86_decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
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

TEST_CASE("store decoder finds the instruction that filled a write watchpoint", "[core][disasm][decoder]")
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

    const auto instruction = mpfshost::disasm::decodeStoreBefore(sequence);

    REQUIRE(instruction.has_value());
    REQUIRE(instruction->offset == 14);
    REQUIRE(instruction->length == 3);
    REQUIRE(instruction->width == 1);
    REQUIRE(instruction->text == "mov byte [rcx], r11b");
}

TEST_CASE("store decoder keeps the operand width", "[core][disasm][decoder]")
{
    SECTION("64-bit register store")
    {
        const auto instruction = mpfshost::disasm::decodeStoreBefore(bytes({0x48, 0x89, 0x18}));
        REQUIRE(instruction.has_value());
        REQUIRE(instruction->width == 8);
        REQUIRE(instruction->text == "mov qword [rax], rbx");
    }

    SECTION("32-bit immediate store")
    {
        const auto instruction = mpfshost::disasm::decodeStoreBefore(bytes({0xC7, 0x00, 0x01, 0x00, 0x00, 0x00}));
        REQUIRE(instruction.has_value());
        REQUIRE(instruction->width == 4);
        REQUIRE(instruction->text == "mov dword [rax], 0x1");
    }

    SECTION("16-bit immediate store")
    {
        const auto instruction = mpfshost::disasm::decodeStoreBefore(bytes({0x66, 0xC7, 0x00, 0x01, 0x00}));
        REQUIRE(instruction.has_value());
        REQUIRE(instruction->width == 2);
        REQUIRE(instruction->text == "mov word [rax], 0x1");
    }

    SECTION("string store")
    {
        const auto instruction = mpfshost::disasm::decodeStoreBefore(bytes({0xF3, 0xAB}));
        REQUIRE(instruction.has_value());
        REQUIRE(instruction->width == 4);
        REQUIRE(instruction->text == "stos dword [rdi]");
    }

    SECTION("rip-relative store")
    {
        const auto instruction = mpfshost::disasm::decodeStoreBefore(bytes({0x48, 0x89, 0x05, 0x10, 0x00, 0x00, 0x00}));
        REQUIRE(instruction.has_value());
        REQUIRE(instruction->text == "mov qword [rip + 0x10], rax");
    }
}

TEST_CASE("disassembler renders lengths and Intel text", "[core][disasm][decoder]")
{
    SECTION("a register move")
    {
        const auto instructions = mpfshost::disasm::disassemble(bytes({0x48, 0x89, 0xD8}), 0x1000, 8);
        REQUIRE(instructions.size() == 1);
        REQUIRE(instructions[0].address == 0x1000);
        REQUIRE(instructions[0].bytes.size() == 3);
        REQUIRE(instructions[0].text.find("mov") != std::string::npos);
        REQUIRE(instructions[0].text.find("rax") != std::string::npos);
        REQUIRE(instructions[0].text.find("rbx") != std::string::npos);
    }

    SECTION("a prefix is part of the instruction")
    {
        const auto instructions = mpfshost::disasm::disassemble(bytes({0xF3, 0x90}), 0x2000, 8);
        REQUIRE(instructions.size() == 1);
        REQUIRE(instructions[0].address == 0x2000);
        REQUIRE(instructions[0].bytes.size() == 2);
        REQUIRE(instructions[0].text == "pause");
    }

    SECTION("several instructions advance the address")
    {
        const auto instructions = mpfshost::disasm::disassemble(bytes({0x90, 0x48, 0x89, 0xD8}), 0x3000, 8);
        REQUIRE(instructions.size() == 2);
        REQUIRE(instructions[0].text == "nop");
        REQUIRE(instructions[1].address == 0x3001);
        REQUIRE(instructions[1].bytes.size() == 3);
    }
}

TEST_CASE("disassembler emits unknown bytes instead of guessing", "[core][disasm][decoder]")
{
    const auto instructions = mpfshost::disasm::disassemble(bytes({0x06, 0x90}), 0x4000, 8);
    REQUIRE(instructions.size() == 2);
    REQUIRE(instructions[0].address == 0x4000);
    REQUIRE(instructions[0].bytes.size() == 1);
    REQUIRE(instructions[0].text == ".byte 0x06");
    REQUIRE(instructions[1].address == 0x4001);
    REQUIRE(instructions[1].text == "nop");
}

TEST_CASE("disassembler truncates at the requested instruction count", "[core][disasm][decoder]")
{
    const auto instructions = mpfshost::disasm::disassemble(bytes({0x90, 0x90, 0x90, 0x90}), 0x5000, 2);
    REQUIRE(instructions.size() == 2);
    REQUIRE(instructions[0].address == 0x5000);
    REQUIRE(instructions[1].address == 0x5001);

    REQUIRE(mpfshost::disasm::disassemble(bytes({0x90}), 0x5000, 0).empty());
    REQUIRE(mpfshost::disasm::disassemble(std::span<const std::byte> {}, 0x5000, 4).empty());
}

TEST_CASE("store decoder rejects loads, comparisons and truncation", "[core][disasm][decoder]")
{
    SECTION("register destination")
    {
        REQUIRE_FALSE(mpfshost::disasm::decodeStoreBefore(bytes({0x48, 0x89, 0xC3})).has_value());
    }

    SECTION("load")
    {
        REQUIRE_FALSE(mpfshost::disasm::decodeStoreBefore(bytes({0x8B, 0x01})).has_value());
    }

    SECTION("comparison does not write memory")
    {
        REQUIRE_FALSE(mpfshost::disasm::decodeStoreBefore(bytes({0x80, 0x38, 0x01})).has_value());
    }

    SECTION("truncated instruction")
    {
        REQUIRE_FALSE(mpfshost::disasm::decodeStoreBefore(bytes({0x44, 0x88})).has_value());
        REQUIRE_FALSE(mpfshost::disasm::decodeStoreBefore(bytes({0x89})).has_value());
    }

    SECTION("no bytes")
    {
        REQUIRE_FALSE(mpfshost::disasm::decodeStoreBefore(std::span<const std::byte> {}).has_value());
    }

    SECTION("non-store instruction")
    {
        REQUIRE_FALSE(mpfshost::disasm::decodeStoreBefore(bytes({0x90})).has_value());
    }
}

TEST_CASE("the disassembler reads a bounded memory window", "[core][disasm][service]")
{
    std::uint64_t readAddress = 0;
    std::uint32_t readSize    = 0;
    const auto    read        = [&](std::uint32_t pid,
                       std::uint64_t address,
                       std::uint32_t size,
                       std::vector<std::byte>& out,
                       std::string&)
    {
        REQUIRE(pid == 42);
        readAddress = address;
        readSize    = size;
        out         = bytes({0x48, 0x89, 0xD8});
        return true;
    };

    std::string error;
    const auto response = mpfshost::disasm::disassembleMemory({42, 0x401000, 3}, read, error);
    REQUIRE(response.has_value());
    REQUIRE(error.empty());
    REQUIRE(readAddress == 0x401000);
    REQUIRE(readSize == 45);
    REQUIRE(response->instructions.size() == 1);
    REQUIRE(response->instructions[0].address == 0x401000);
    REQUIRE(response->instructions[0].text.find("mov") != std::string::npos);

    const auto failing = [](std::uint32_t, std::uint64_t, std::uint32_t, std::vector<std::byte>&, std::string& text)
    {
        text = "unmapped";
        return false;
    };
    REQUIRE_FALSE(mpfshost::disasm::disassembleMemory({42, 0x401000, 3}, failing, error).has_value());
    REQUIRE(error == "unmapped");

    const auto empty = [](std::uint32_t, std::uint64_t, std::uint32_t, std::vector<std::byte>&, std::string&)
    {
        return true;
    };
    REQUIRE_FALSE(mpfshost::disasm::disassembleMemory({42, 0x401000, 3}, empty, error).has_value());
    REQUIRE(error == "the requested memory is empty");

    REQUIRE_FALSE(mpfshost::disasm::disassembleMemory({42, 0x401000, 0}, read, error).has_value());
    REQUIRE(error == "the disassembly instruction count is out of range");
}

TEST_CASE("disassemble messages round trip", "[core][disasm][codec]")
{
    const auto request = mpfshost::decodeDisassembleRequest(mpfshost::encodeDisassembleRequest({7, 0x401000, 12}));
    REQUIRE(request.has_value());
    REQUIRE(request->pid == 7);
    REQUIRE(request->address == 0x401000);
    REQUIRE(request->count == 12);
    REQUIRE_FALSE(mpfshost::decodeDisassembleRequest({}).has_value());
    REQUIRE_FALSE(mpfshost::decodeDisassembleRequest(mpfshost::encodeDisassembleRequest({7, 0x401000, 0})).has_value());
    REQUIRE_FALSE(
        mpfshost::decodeDisassembleRequest(
            mpfshost::encodeDisassembleRequest({7, 0x401000, mpfshost::kMaxDisassembleInstructions + 1}))
            .has_value());

    mpfshost::InstructionRecord instruction;
    instruction.address = 0x401000;
    instruction.bytes   = {std::byte {0x48}, std::byte {0x89}, std::byte {0xD8}};
    instruction.text    = "mov rax, rbx";
    const auto decoded =
        mpfshost::decodeDisassembleResponse(mpfshost::encodeDisassembleResponse({{instruction}}));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->instructions.size() == 1);
    REQUIRE(decoded->instructions[0].address == 0x401000);
    REQUIRE(decoded->instructions[0].bytes.size() == 3);
    REQUIRE(decoded->instructions[0].text == "mov rax, rbx");
    REQUIRE_FALSE(mpfshost::decodeDisassembleResponse({}).has_value());
}
