#include "core/server/session.hpp"

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"
#include "core/server/keyboard_provider.hpp"

namespace mpfshost
{
    void Session::handleKeyboardState(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeKeyboardStateRequest(payload);
        if (!request)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed keyboard state request");
            return;
        }

        auto* provider = dynamic_cast<KeyboardProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot read the keyboard state");
            return;
        }

        std::string error;
        bool        unavailable = false;
        const auto  response    = provider->keyboardState(*request, error, unavailable);
        if (!response)
        {
            if (unavailable)
            {
                sendError(requestId,
                          ErrorCode::CapabilityUnavailable,
                          error.empty() ? "the keyboard state is unavailable" : error);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "reading the keyboard state failed" : error);
            return;
        }
        sendPayload(requestId, MessageType::KeyboardStateResponse, encodeKeyboardStateResponse(*response));
    }
} // namespace mpfshost
