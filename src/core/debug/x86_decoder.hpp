#ifndef MPFSHOST_CORE_DEBUG_X86_DECODER_HPP
#define MPFSHOST_CORE_DEBUG_X86_DECODER_HPP

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace mpfshost::debug
{
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
