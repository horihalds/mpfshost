#ifndef MPFSHOST_CORE_DISASM_DISASSEMBLER_HPP
#define MPFSHOST_CORE_DISASM_DISASSEMBLER_HPP

#pragma once

#include "core/ipc/message.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mpfshost::disasm
{
    // Reads process memory for the disassembler. The host injects its memory path; tests inject a buffer.
    using ReadMemory = std::function<bool(
        std::uint32_t pid, std::uint64_t address, std::uint32_t size, std::vector<std::byte>& out, std::string& error)>;

    // Reads one bounded window through `read` and decodes it. This needs no debugger stub and no paused guest,
    // so it works while the target runs. A failed or empty read is reported through `error`.
    [[nodiscard]] std::optional<DisassembleResponse>
    disassembleMemory(const DisassembleRequest& request, const ReadMemory& read, std::string& error);
} // namespace mpfshost::disasm

#endif // MPFSHOST_CORE_DISASM_DISASSEMBLER_HPP
