#ifndef MPFSHOST_CORE_SERVER_DEBUG_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_DEBUG_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    using DebugWatchHitSink       = std::function<bool(const DebugWatchData&)>;
    using DebugWatchHeartbeatSink = std::function<bool(const DebugWatchHeartbeat&)>;
    using DebugWatchAbortCheck    = std::function<bool()>;
    using DebugWaitHitSink        = std::function<bool(const DebugWaitHit&)>;

    // Guest control through QEMU's QMP socket. Every call answers the complete debug status, so a control
    // request does not need a second round trip to report the resulting run state.
    class DebugProvider
    {
    public:
        virtual ~DebugProvider() = default;

        // `busy` reports another debug operation holding the target, which the session maps to ErrorCode::Busy.
        // `unavailable` reports that QMP could not be reached, which the session maps to CapabilityUnavailable.
        [[nodiscard]] virtual std::optional<DebugStatus>
        debugState(bool& busy, std::string& error, bool& unavailable) = 0;
        [[nodiscard]] virtual std::optional<DebugStatus>
        debugControl(DebugControlAction action, bool& busy, std::string& error, bool& unavailable) = 0;
        // `invalidState` reports a running guest or another owner's stub, which the session maps to
        // ErrorCode::InvalidState instead of Internal.
        [[nodiscard]] virtual std::optional<std::vector<DebugRegisterRecord>>
        debugRegisters(const DebugRegistersRequest& request,
                       bool&                        invalidState,
                       bool&                        busy,
                       std::string&                 error,
                       bool&                        unavailable) = 0;
        // Lists the stub's threads with their current context and, where possible, the guest pid and tid.
        [[nodiscard]] virtual std::optional<std::vector<DebugThreadRecord>>
                                                                    debugThreads(const DebugThreadsRequest& request,
                                                                                 bool&                      invalidState,
                                                                                 bool&                      busy,
                                                                                 std::string&               error,
                                                                                 bool&                      unavailable) = 0;
        // Reads one stopped thread and walks its frame-pointer chain. `invalidState` reports a running guest,
        // another owner's stub, or an unknown thread, which the session maps to InvalidState.
        [[nodiscard]] virtual std::optional<DebugBacktraceResponse> debugBacktrace(const DebugBacktraceRequest& request,
                                                                                   bool&        invalidState,
                                                                                   bool&        busy,
                                                                                   std::string& error,
                                                                                   bool&        unavailable)   = 0;
        // `onHit` sends one data frame and returns false when it could not be written; `onHeartbeat` does the
        // same for the periodic status frame; `aborted` is polled between short stops so cancellation, the
        // request deadline and peer loss all end the watch quickly.
        [[nodiscard]] virtual std::optional<DebugWatchEnd> debugWatch(const DebugWatchRequest&       request,
                                                                      const DebugWatchHitSink&       onHit,
                                                                      const DebugWatchHeartbeatSink& onHeartbeat,
                                                                      const DebugWatchAbortCheck&    aborted,
                                                                      bool&                          busy,
                                                                      std::string&                   error,
                                                                      bool&                          unavailable,
                                                                      bool&                          notFound) = 0;
        // `invalidState` reports a running guest or another owner's stub when a request needs a paused guest.
        // `notFound` reports a module or symbol that could not be resolved, which the session maps to NotFound.
        [[nodiscard]] virtual std::optional<DebugBreakpointResponse>
                                                               debugBreakpoint(const DebugBreakpointRequest& request,
                                                                               const DebugWatchAbortCheck&   aborted,
                                                                               bool&                         invalidState,
                                                                               bool&                         busy,
                                                                               std::string&                  error,
                                                                               bool&                         unavailable,
                                                                               bool&                         notFound) = 0;
        [[nodiscard]] virtual std::optional<DebugStepResponse> debugStep(
            const DebugStepRequest& request, bool& invalidState, bool& busy, std::string& error, bool& unavailable) = 0;
        // Starts one persistent stub for `sessionId`. `invalidState` reports a foreign stub, which the session
        // maps to InvalidState together with the debug-detach hint.
        [[nodiscard]] virtual std::optional<DebugAttachResponse>
        debugAttach(std::uint64_t sessionId, bool& invalidState, bool& busy, std::string& error, bool& unavailable) = 0;
        // Stops the attached or foreign stub, including one this host did not start, and resumes the guest only
        // when this host paused it for the attachment.
        [[nodiscard]] virtual std::optional<DebugDetachResponse>
                     debugDetach(std::uint64_t sessionId, bool& busy, std::string& error, bool& unavailable) = 0;
        // Best-effort teardown of an attachment owned by a closing session. Never throws.
        virtual void debugDetachSession(std::uint64_t sessionId) noexcept                                    = 0;

        // Persistent resources. `invalidState` reports a missing attachment, another owner's attachment, a
        // running guest or an exhausted hardware-slot budget.
        [[nodiscard]] virtual std::optional<DebugBreakpointArmResponse>
        debugArmBreakpoint(std::uint64_t                    sessionId,
                           const DebugBreakpointArmRequest& request,
                           bool&                            invalidState,
                           bool&                            busy,
                           std::string&                     error,
                           bool&                            unavailable) = 0;
        [[nodiscard]] virtual std::optional<DebugResourceRemoveResponse>
        debugRemoveBreakpoint(std::uint64_t                       sessionId,
                              const DebugBreakpointRemoveRequest& request,
                              bool&                               invalidState,
                              bool&                               busy,
                              std::string&                        error,
                              bool&                               unavailable) = 0;
        [[nodiscard]] virtual std::optional<DebugBreakpointListResponse> debugListBreakpoints(
            std::uint64_t sessionId, bool& invalidState, bool& busy, std::string& error, bool& unavailable) = 0;
        [[nodiscard]] virtual std::optional<DebugWatchArmResponse> debugArmWatch(std::uint64_t               sessionId,
                                                                                 const DebugWatchArmRequest& request,
                                                                                 bool&        invalidState,
                                                                                 bool&        busy,
                                                                                 std::string& error,
                                                                                 bool&        unavailable)  = 0;
        [[nodiscard]] virtual std::optional<DebugResourceRemoveResponse>
        debugRemoveWatch(std::uint64_t                  sessionId,
                         const DebugWatchRemoveRequest& request,
                         bool&                          invalidState,
                         bool&                          busy,
                         std::string&                   error,
                         bool&                          unavailable) = 0;
        [[nodiscard]] virtual std::optional<DebugWatchListResponse> debugListWatches(
            std::uint64_t sessionId, bool& invalidState, bool& busy, std::string& error, bool& unavailable) = 0;
        [[nodiscard]] virtual std::optional<DebugContinueResponse> debugContinueGuest(
            std::uint64_t sessionId, bool& invalidState, bool& busy, std::string& error, bool& unavailable)  = 0;
        [[nodiscard]] virtual std::optional<DebugWaitEnd> debugWait(const DebugWaitRequest&     request,
                                                                    const DebugWaitHitSink&     onHit,
                                                                    const DebugWatchAbortCheck& aborted,
                                                                    std::uint64_t               sessionId,
                                                                    bool&                       invalidState,
                                                                    bool&                       busy,
                                                                    std::string&                error,
                                                                    bool&                       unavailable) = 0;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_DEBUG_PROVIDER_HPP
