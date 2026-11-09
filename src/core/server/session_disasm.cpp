#include "core/server/session.hpp"

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"
#include "core/server/disasm_provider.hpp"

namespace mpfshost
{
    void Session::handleDisassemble(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeDisassembleRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed disassemble request");
            return;
        }
        if (request->pid == 0)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "a disassemble request needs a process id");
            return;
        }

        auto* provider = dynamic_cast<DisasmProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot disassemble memory");
            return;
        }

        std::string error;
        const auto  response = provider->disassemble(*request, error);
        if (!response.has_value())
        {
            sendError(requestId, ErrorCode::Internal, error.empty() ? "disassembly failed" : error);
            return;
        }
        sendPayload(requestId, MessageType::DisassembleResponse, encodeDisassembleResponse(*response));
    }
} // namespace mpfshost
