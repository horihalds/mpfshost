#ifndef MPFSHOST_CORE_DEBUG_X86_DECODER_HPP
#define MPFSHOST_CORE_DEBUG_X86_DECODER_HPP

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mpfshost::debug
{
    inline constexpr std::size_t kMaxDisassemblyInstructions = 128;
    inline constexpr std::size_t kMaxDisassemblyBytes        = kMaxDisassemblyInstructions * 15;

    struct DisassembledInstruction
    {
        std::uint64_t          address {};
        std::vector<std::byte> bytes;
        std::string            text; // Intel-syntax rendering, or ".byte 0xNN" for an unknown encoding
    };

    // Decodes at most `maxInstructions` instructions from `bytes`, starting at the virtual `address`. An
    // instruction Zydis cannot decode becomes one `.byte 0xNN` line and consumes exactly one byte, so a
    // caller never receives a guessed length. The caller is responsible for keeping `maxInstructions`
    // within kMaxDisassemblyInstructions.
    [[nodiscard]] std::vector<DisassembledInstruction>
    disassemble(std::span<const std::byte> bytes, std::uint64_t address, std::size_t maxInstructions);

    struct StoreInstruction
    {
        std::size_t   offset {}; // byte offset of the instruction inside the supplied buffer
        std::size_t   length {};
        std::uint32_t width {}; // number of bytes written to memory
        std::string   text;     // Intel-syntax rendering, for example "mov byte [rcx], r11b"
    };

    // x86-64 data breakpoints are trap-after: `rip` already points past the store. This decodes the last
    // instruction in `bytesBefore` and returns it only when that instruction is a plain memory write. The
    // decoder is intentionally small; nullopt means "do not guess", not "there was no write".
    [[nodiscard]] std::optional<StoreInstruction> decodeStoreBefore(std::span<const std::byte> bytesBefore);
} // namespace mpfshost::debug

#endif // MPFSHOST_CORE_DEBUG_X86_DECODER_HPP
