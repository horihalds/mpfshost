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
        bool        busy   = false;
        auto        status = provider->debugState(busy, error);
        if (!status.has_value())
        {
            if (busy)
            {
                sendError(requestId, ErrorCode::Busy, error.empty() ? "another debug operation is in progress" : error);
                return;
            }
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
        bool        busy   = false;
        auto        status = provider->debugControl(request->action, busy, error);
        if (!status.has_value())
        {
            if (busy)
            {
                sendError(requestId, ErrorCode::Busy, error.empty() ? "another debug operation is in progress" : error);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "debug control failed" : error);
            return;
        }
        sendPayload(requestId, MessageType::DebugControlResponse, encodeDebugControlResponse(*status));
    }

    void Session::handleDebugRegisters(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeDebugRegistersRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed debug registers request");
            return;
        }
        if (request->vcpu != kDebugAllVcpus && request->vcpu >= kMaxDebugVcpuRecords)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "the vCPU index is out of range");
            return;
        }

        auto* provider = dynamic_cast<DebugProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot read debug registers");
            return;
        }

        bool        invalidState = false;
        bool        busy         = false;
        std::string error;
        auto        records = provider->debugRegisters(*request, invalidState, busy, error);
        if (!records.has_value())
        {
            if (busy)
            {
                sendError(requestId, ErrorCode::Busy, error.empty() ? "another debug operation is in progress" : error);
                return;
            }
            if (invalidState)
            {
                sendError(requestId,
                          ErrorCode::InvalidState,
                          error.empty() ? "the guest is not in a state that allows this request" : error);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "debug register read failed" : error);
            return;
        }

        sendPayload(requestId, MessageType::DebugRegistersResponse, encodeDebugRegistersResponse({*records}));
    }

    void Session::handleDebugWatch(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeDebugWatchRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed debug watch request");
            return;
        }
        if (request->pid == 0)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "a debug watch needs a process id");
            return;
        }
        if (request->address == 0)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "a debug watch needs a nonzero address");
            return;
        }
        if (request->width != 1 && request->width != 2 && request->width != 4 && request->width != 8)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "the watch width must be 1, 2, 4 or 8 bytes");
            return;
        }
        if (request->maxHits == 0 || request->maxHits > kMaxDebugWatchHits)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "the hit limit is out of range");
            return;
        }
        if (request->timeoutMs == 0 || request->timeoutMs > kMaxDebugWatchTimeoutMs)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "the watch timeout is out of range");
            return;
        }

        auto* provider = dynamic_cast<DebugProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot run a debug watch");
            return;
        }

        bool                       aborted    = false;
        const DebugWatchAbortCheck abortCheck = [&]()
        {
            if (aborted)
            {
                return true;
            }
            ErrorCode code {};
            if (requestAborted(requestId, code))
            {
                aborted = true;
                return true;
            }
            if (peerGone())
            {
                aborted      = true;
                _writeFailed = true;
                return true;
            }
            return false;
        };
        const DebugWatchHitSink onHit = [&](const DebugWatchData& hit)
        {
            if (abortCheck())
            {
                return false;
            }
            sendPayload(requestId, MessageType::DebugWatchData, encodeDebugWatchData(hit));
            if (_writeFailed)
            {
                aborted = true;
                return false;
            }
            return true;
        };

        bool        busy = false;
        std::string error;
        const auto  end = provider->debugWatch(*request, onHit, abortCheck, busy, error);
        if (!end.has_value())
        {
            if (busy)
            {
                sendError(requestId, ErrorCode::Busy, error.empty() ? "another debug operation is in progress" : error);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "the debug watch failed" : error);
            return;
        }
        if (aborted)
        {
            return;
        }
        sendPayload(requestId, MessageType::DebugWatchEnd, encodeDebugWatchEnd(*end));
    }

    void Session::handleDebugBreakpoint(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeDebugBreakpointRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed debug breakpoint request");
            return;
        }
        if (request->pid == 0)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "a debug breakpoint needs a process id");
            return;
        }
        if (request->address == 0)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "a debug breakpoint needs a nonzero address");
            return;
        }
        if (request->timeoutMs == 0 || request->timeoutMs > kMaxDebugWatchTimeoutMs)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "the breakpoint timeout is out of range");
            return;
        }

        auto* provider = dynamic_cast<DebugProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot set a debug breakpoint");
            return;
        }

        bool                       aborted    = false;
        const DebugWatchAbortCheck abortCheck = [&]()
        {
            if (aborted)
            {
                return true;
            }
            ErrorCode code {};
            if (requestAborted(requestId, code))
            {
                aborted = true;
                return true;
            }
            if (peerGone())
            {
                aborted      = true;
                _writeFailed = true;
                return true;
            }
            return false;
        };

        bool        invalidState = false;
        bool        busy         = false;
        std::string error;
        const auto  response = provider->debugBreakpoint(*request, abortCheck, invalidState, busy, error);
        if (!response.has_value())
        {
            if (busy)
            {
                sendError(requestId, ErrorCode::Busy, error.empty() ? "another debug operation is in progress" : error);
                return;
            }
            if (invalidState)
            {
                sendError(requestId,
                          ErrorCode::InvalidState,
                          error.empty() ? "the guest is not in a state that allows this request" : error);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "the debug breakpoint failed" : error);
            return;
        }
        if (aborted)
        {
            return;
        }
        sendPayload(requestId, MessageType::DebugBreakpointResponse, encodeDebugBreakpointResponse(*response));
    }

    void Session::handleDebugStep(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeDebugStepRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed debug step request");
            return;
        }
        if (request->vcpu >= kMaxDebugVcpuRecords)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "the vCPU index is out of range");
            return;
        }

        auto* provider = dynamic_cast<DebugProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot single-step the guest");
            return;
        }

        bool        invalidState = false;
        bool        busy         = false;
        std::string error;
        const auto  response = provider->debugStep(*request, invalidState, busy, error);
        if (!response.has_value())
        {
            if (busy)
            {
                sendError(requestId, ErrorCode::Busy, error.empty() ? "another debug operation is in progress" : error);
                return;
            }
            if (invalidState)
            {
                sendError(requestId,
                          ErrorCode::InvalidState,
                          error.empty() ? "the guest is not in a state that allows this request" : error);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "the single step failed" : error);
            return;
        }
        sendPayload(requestId, MessageType::DebugStepResponse, encodeDebugStepResponse(*response));
    }

    void Session::handleDebugDetach(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeDebugDetachRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "debug-detach takes no payload");
            return;
        }

        auto* provider = dynamic_cast<DebugProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot detach the debugger");
            return;
        }

        bool        busy = false;
        std::string error;
        const auto  response = provider->debugDetach(busy, error);
        if (!response.has_value())
        {
            if (busy)
            {
                sendError(requestId, ErrorCode::Busy, error.empty() ? "another debug operation is in progress" : error);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "debug detach failed" : error);
            return;
        }
        sendPayload(requestId, MessageType::DebugDetachResponse, encodeDebugDetachResponse(*response));
    }
} // namespace mpfshost
