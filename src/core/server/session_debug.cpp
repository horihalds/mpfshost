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
        bool        busy        = false;
        bool        unavailable = false;
        auto        status      = provider->debugState(busy, error, unavailable);
        if (!status.has_value())
        {
            if (busy)
            {
                sendError(requestId, ErrorCode::Busy, error.empty() ? "another debug operation is in progress" : error);
                return;
            }
            if (unavailable)
            {
                sendError(requestId,
                          ErrorCode::CapabilityUnavailable,
                          error.empty() ? "the debug channel is unavailable" : error);
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
        bool        busy        = false;
        bool        unavailable = false;
        auto        status      = provider->debugControl(request->action, busy, error, unavailable);
        if (!status.has_value())
        {
            if (busy)
            {
                sendError(requestId, ErrorCode::Busy, error.empty() ? "another debug operation is in progress" : error);
                return;
            }
            if (unavailable)
            {
                sendError(requestId,
                          ErrorCode::CapabilityUnavailable,
                          error.empty() ? "the debug channel is unavailable" : error);
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
        bool        unavailable = false;
        auto        records     = provider->debugRegisters(*request, invalidState, busy, error, unavailable);
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
            if (unavailable)
            {
                sendError(requestId,
                          ErrorCode::CapabilityUnavailable,
                          error.empty() ? "the debug channel is unavailable" : error);
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
        bool        unavailable = false;
        const auto  end         = provider->debugWatch(*request, onHit, abortCheck, busy, error, unavailable);
        if (!end.has_value())
        {
            if (aborted)
            {
                provider->debugDetachSession(_bound->sessionId);
                return;
            }
            if (busy)
            {
                sendError(requestId, ErrorCode::Busy, error.empty() ? "another debug operation is in progress" : error);
                return;
            }
            if (unavailable)
            {
                sendError(requestId,
                          ErrorCode::CapabilityUnavailable,
                          error.empty() ? "the debug channel is unavailable" : error);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "the debug watch failed" : error);
            return;
        }
        if (aborted)
        {
            provider->debugDetachSession(_bound->sessionId);
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
        bool        unavailable = false;
        const auto  response = provider->debugBreakpoint(*request, abortCheck, invalidState, busy, error, unavailable);
        if (!response.has_value())
        {
            if (aborted)
            {
                provider->debugDetachSession(_bound->sessionId);
                return;
            }
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
            if (unavailable)
            {
                sendError(requestId,
                          ErrorCode::CapabilityUnavailable,
                          error.empty() ? "the debug channel is unavailable" : error);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "the debug breakpoint failed" : error);
            return;
        }
        if (aborted)
        {
            provider->debugDetachSession(_bound->sessionId);
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
        bool        unavailable = false;
        const auto  response    = provider->debugStep(*request, invalidState, busy, error, unavailable);
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
            if (unavailable)
            {
                sendError(requestId,
                          ErrorCode::CapabilityUnavailable,
                          error.empty() ? "the debug channel is unavailable" : error);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "the single step failed" : error);
            return;
        }
        sendPayload(requestId, MessageType::DebugStepResponse, encodeDebugStepResponse(*response));
    }

    void Session::handleDebugAttach(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeDebugAttachRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "debug-attach takes no payload");
            return;
        }

        auto* provider = dynamic_cast<DebugProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot attach the debugger");
            return;
        }

        bool        invalidState = false;
        bool        busy         = false;
        std::string error;
        bool        unavailable = false;
        const auto  response    = provider->debugAttach(_bound->sessionId, invalidState, busy, error, unavailable);
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
                          error.empty() ? "the guest is not in a state that allows an attach" : error);
                return;
            }
            if (unavailable)
            {
                sendError(requestId,
                          ErrorCode::CapabilityUnavailable,
                          error.empty() ? "the debug channel is unavailable" : error);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "debug attach failed" : error);
            return;
        }
        sendPayload(requestId, MessageType::DebugAttachResponse, encodeDebugAttachResponse(*response));
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
        bool        unavailable = false;
        const auto  response    = provider->debugDetach(_bound->sessionId, busy, error, unavailable);
        if (!response.has_value())
        {
            if (busy)
            {
                sendError(requestId, ErrorCode::Busy, error.empty() ? "another debug operation is in progress" : error);
                return;
            }
            if (unavailable)
            {
                sendError(requestId,
                          ErrorCode::CapabilityUnavailable,
                          error.empty() ? "the debug channel is unavailable" : error);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "debug detach failed" : error);
            return;
        }
        sendPayload(requestId, MessageType::DebugDetachResponse, encodeDebugDetachResponse(*response));
    }

    void Session::handleDebugBreakpointArm(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeDebugBreakpointArmRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed debug breakpoint arm request");
            return;
        }
        if (request->pid == 0)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "a persistent breakpoint needs a process id");
            return;
        }
        if (request->address == 0)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "a persistent breakpoint needs a nonzero address");
            return;
        }

        auto* provider = dynamic_cast<DebugProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot arm a debug breakpoint");
            return;
        }

        bool        invalidState = false;
        bool        busy         = false;
        std::string error;
        bool        unavailable = false;
        const auto  response =
            provider->debugArmBreakpoint(_bound->sessionId, *request, invalidState, busy, error, unavailable);
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
            if (unavailable)
            {
                sendError(requestId,
                          ErrorCode::CapabilityUnavailable,
                          error.empty() ? "the debug channel is unavailable" : error);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "debug breakpoint arm failed" : error);
            return;
        }
        sendPayload(requestId, MessageType::DebugBreakpointArmResponse, encodeDebugBreakpointArmResponse(*response));
    }

    void Session::handleDebugBreakpointRemove(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeDebugBreakpointRemoveRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed debug breakpoint remove request");
            return;
        }

        auto* provider = dynamic_cast<DebugProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot remove a debug breakpoint");
            return;
        }

        bool        invalidState = false;
        bool        busy         = false;
        std::string error;
        bool        unavailable = false;
        const auto  response =
            provider->debugRemoveBreakpoint(_bound->sessionId, *request, invalidState, busy, error, unavailable);
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
            if (unavailable)
            {
                sendError(requestId,
                          ErrorCode::CapabilityUnavailable,
                          error.empty() ? "the debug channel is unavailable" : error);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "debug breakpoint remove failed" : error);
            return;
        }
        sendPayload(requestId, MessageType::DebugResourceRemoveResponse, encodeDebugResourceRemoveResponse(*response));
    }

    void Session::handleDebugBreakpointList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeDebugBreakpointListRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "debug-breakpoint-list takes no payload");
            return;
        }

        auto* provider = dynamic_cast<DebugProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list debug breakpoints");
            return;
        }

        bool        invalidState = false;
        bool        busy         = false;
        std::string error;
        bool        unavailable = false;
        const auto response = provider->debugListBreakpoints(_bound->sessionId, invalidState, busy, error, unavailable);
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
            if (unavailable)
            {
                sendError(requestId,
                          ErrorCode::CapabilityUnavailable,
                          error.empty() ? "the debug channel is unavailable" : error);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "debug breakpoint list failed" : error);
            return;
        }
        sendPayload(requestId, MessageType::DebugBreakpointListResponse, encodeDebugBreakpointListResponse(*response));
    }

    void Session::handleDebugWatchArm(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeDebugWatchArmRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed debug watch arm request");
            return;
        }
        if (request->pid == 0)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "a persistent watchpoint needs a process id");
            return;
        }
        if (request->address == 0)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "a persistent watchpoint needs a nonzero address");
            return;
        }
        if (request->width != 1 && request->width != 2 && request->width != 4 && request->width != 8)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "the watch width must be 1, 2, 4 or 8 bytes");
            return;
        }

        auto* provider = dynamic_cast<DebugProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot arm a debug watchpoint");
            return;
        }

        bool        invalidState = false;
        bool        busy         = false;
        std::string error;
        bool        unavailable = false;
        const auto  response =
            provider->debugArmWatch(_bound->sessionId, *request, invalidState, busy, error, unavailable);
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
            if (unavailable)
            {
                sendError(requestId,
                          ErrorCode::CapabilityUnavailable,
                          error.empty() ? "the debug channel is unavailable" : error);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "debug watchpoint arm failed" : error);
            return;
        }
        sendPayload(requestId, MessageType::DebugWatchArmResponse, encodeDebugWatchArmResponse(*response));
    }

    void Session::handleDebugWatchRemove(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeDebugWatchRemoveRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed debug watch remove request");
            return;
        }

        auto* provider = dynamic_cast<DebugProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot remove a debug watchpoint");
            return;
        }

        bool        invalidState = false;
        bool        busy         = false;
        std::string error;
        bool        unavailable = false;
        const auto  response =
            provider->debugRemoveWatch(_bound->sessionId, *request, invalidState, busy, error, unavailable);
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
            if (unavailable)
            {
                sendError(requestId,
                          ErrorCode::CapabilityUnavailable,
                          error.empty() ? "the debug channel is unavailable" : error);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "debug watchpoint remove failed" : error);
            return;
        }
        sendPayload(requestId, MessageType::DebugResourceRemoveResponse, encodeDebugResourceRemoveResponse(*response));
    }

    void Session::handleDebugWatchList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeDebugWatchListRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "debug-watch-list takes no payload");
            return;
        }

        auto* provider = dynamic_cast<DebugProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list debug watchpoints");
            return;
        }

        bool        invalidState = false;
        bool        busy         = false;
        std::string error;
        bool        unavailable = false;
        const auto  response    = provider->debugListWatches(_bound->sessionId, invalidState, busy, error, unavailable);
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
            if (unavailable)
            {
                sendError(requestId,
                          ErrorCode::CapabilityUnavailable,
                          error.empty() ? "the debug channel is unavailable" : error);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "debug watchpoint list failed" : error);
            return;
        }
        sendPayload(requestId, MessageType::DebugWatchListResponse, encodeDebugWatchListResponse(*response));
    }

    void Session::handleDebugContinue(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeDebugContinueRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "debug-continue-armed takes no payload");
            return;
        }

        auto* provider = dynamic_cast<DebugProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot continue the guest");
            return;
        }

        bool        invalidState = false;
        bool        busy         = false;
        std::string error;
        bool        unavailable = false;
        const auto  response = provider->debugContinueGuest(_bound->sessionId, invalidState, busy, error, unavailable);
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
            if (unavailable)
            {
                sendError(requestId,
                          ErrorCode::CapabilityUnavailable,
                          error.empty() ? "the debug channel is unavailable" : error);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "debug continue failed" : error);
            return;
        }
        sendPayload(requestId, MessageType::DebugContinueResponse, encodeDebugContinueResponse(*response));
    }

    void Session::handleDebugWait(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeDebugWaitRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed debug wait request");
            return;
        }
        if (request->timeoutMs == 0 || request->timeoutMs > kMaxDebugWatchTimeoutMs)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "the debug wait timeout is out of range");
            return;
        }
        if (request->maxHits == 0 || request->maxHits > kMaxDebugWaitHits)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "the debug wait hit limit is out of range");
            return;
        }

        auto* provider = dynamic_cast<DebugProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot wait for debug hits");
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
        const DebugWaitHitSink onHit = [&](const DebugWaitHit& hit)
        {
            if (abortCheck())
            {
                return false;
            }
            sendPayload(requestId, MessageType::DebugWaitHit, encodeDebugWaitHit(hit));
            if (_writeFailed)
            {
                aborted = true;
                return false;
            }
            return true;
        };

        bool        invalidState = false;
        bool        busy         = false;
        std::string error;
        bool        unavailable = false;
        const auto  end =
            provider->debugWait(*request, onHit, abortCheck, _bound->sessionId, invalidState, busy, error, unavailable);
        if (!end.has_value())
        {
            if (aborted)
            {
                provider->debugDetachSession(_bound->sessionId);
                return;
            }
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
            if (unavailable)
            {
                sendError(requestId,
                          ErrorCode::CapabilityUnavailable,
                          error.empty() ? "the debug channel is unavailable" : error);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "debug wait failed" : error);
            return;
        }
        if (aborted)
        {
            provider->debugDetachSession(_bound->sessionId);
            return;
        }
        sendPayload(requestId, MessageType::DebugWaitEnd, encodeDebugWaitEnd(*end));
    }
} // namespace mpfshost
