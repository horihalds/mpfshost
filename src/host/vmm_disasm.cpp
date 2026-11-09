#include "host/vmm_handle.hpp"

#include "core/disasm/disassembler.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mpfshost
{
    std::optional<DisassembleResponse> VmmHandle::disassemble(const DisassembleRequest& request, std::string& error)
    {
        return disasm::disassembleMemory(
            request,
            [this](std::uint32_t           pid,
                   std::uint64_t           address,
                   std::uint32_t           size,
                   std::vector<std::byte>& out,
                   std::string&            readError)
            {
                return readMemory(pid, address, size, out, readError);
            },
            error);
    }
} // namespace mpfshost
