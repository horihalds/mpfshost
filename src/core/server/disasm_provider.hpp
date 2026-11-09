#ifndef MPFSHOST_CORE_SERVER_DISASM_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_DISASM_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"

#include <optional>
#include <string>

namespace mpfshost
{
    // Decodes process memory without touching a debugger stub, so it answers while the guest runs.
    class DisasmProvider
    {
    public:
        virtual ~DisasmProvider() = default;

        [[nodiscard]] virtual std::optional<DisassembleResponse> disassemble(const DisassembleRequest& request,
                                                                             std::string&              error) = 0;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_DISASM_PROVIDER_HPP
