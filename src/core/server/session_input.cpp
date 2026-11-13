#include "core/server/session.hpp"

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"
#include "core/server/input_provider.hpp"

namespace mpfshost
{
    void Session::handleInput(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeInputEventRequest(payload);
        if (!request)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed input event request");
            return;
        }

        auto* provider = dynamic_cast<InputProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot inject input");
            return;
        }

        std::string error;
        bool        unavailable = false;
        const auto  response    = provider->input(*request, error, unavailable);
        if (!response)
        {
            if (unavailable)
            {
                sendError(requestId,
                          ErrorCode::CapabilityUnavailable,
                          error.empty() ? "the input channel is unavailable" : error);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "input injection failed" : error);
            return;
        }
        sendPayload(requestId, MessageType::InputEventResponse, encodeInputEventResponse(*response));
    }
} // namespace mpfshost
