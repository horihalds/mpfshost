#include "core/disasm/disassembler.hpp"

#include "core/disasm/x86_decoder.hpp"

namespace mpfshost::disasm
{
    std::optional<DisassembleResponse>
    disassembleMemory(const DisassembleRequest& request, const ReadMemory& read, std::string& error)
    {
        if (request.count == 0 || request.count > kMaxDisassembleInstructions)
        {
            error = "the disassembly instruction count is out of range";
            return std::nullopt;
        }

        // Never read more than the decoder can consume, even for a maximum-length instruction count.
        const auto wanted = request.count * 15u;
        const auto size   = wanted < kMaxDisassembleBytes ? wanted : kMaxDisassembleBytes;

        std::vector<std::byte> bytes;
        if (!read(request.pid, request.address, size, bytes, error))
        {
            return std::nullopt;
        }
        if (bytes.empty())
        {
            error = "the requested memory is empty";
            return std::nullopt;
        }

        const auto          decoded = disassemble(bytes, request.address, request.count);
        DisassembleResponse response;
        response.instructions.reserve(decoded.size());
        for (const auto& instruction : decoded)
        {
            response.instructions.push_back({instruction.address, instruction.bytes, instruction.text});
        }
        return response;
    }
} // namespace mpfshost::disasm
