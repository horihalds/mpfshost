#include "core/server/session.hpp"

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"
#include "core/server/debug_provider.hpp"

namespace mpfshost
{
    void Session::handleDebugState(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }
        if (!payload.empty())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "debug-state takes no payload");
            return;
        }

        auto* provider = dynamic_cast<DebugProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot report debug state");
            return;
        }

        std::string error;
        auto        status = provider->debugState(error);
        if (!status.has_value())
        {
            sendError(requestId, ErrorCode::Internal, error.empty() ? "debug state query failed" : error);
            return;
        }
        sendPayload(requestId, MessageType::DebugStateResponse, encodeDebugStateResponse(*status));
    }

    void Session::handleDebugControl(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeDebugControlRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed debug control request");
            return;
        }

        auto* provider = dynamic_cast<DebugProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot control debug state");
            return;
        }

        std::string error;
        auto        status = provider->debugControl(request->action, error);
        if (!status.has_value())
        {
            sendError(requestId, ErrorCode::Internal, error.empty() ? "debug control failed" : error);
            return;
        }
        sendPayload(requestId, MessageType::DebugControlResponse, encodeDebugControlResponse(*status));
    }
} // namespace mpfshost
