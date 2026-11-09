#ifndef MPFSHOST_CORE_DEBUG_DEBUG_CONTROLLER_HPP
#define MPFSHOST_CORE_DEBUG_DEBUG_CONTROLLER_HPP

#pragma once

#include "core/debug/debug_inspector.hpp"
#include "core/debug/qmp_client.hpp"
#include "core/debug/rsp_client.hpp"
#include "core/ipc/message.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mpfshost::debug
{
    using QmpTransportFactory  = std::move_only_function<std::unique_ptr<QmpTransport>()>;
    using RspTransportFactory  = std::move_only_function<std::unique_ptr<RspTransport>()>;
    using DebugWatchHitSink    = std::function<bool(const DebugWatchData&)>;
    using DebugWatchAbortCheck = std::function<bool()>;
    using DebugWaitHitSink     = std::function<bool(const DebugWaitHit&)>;

    // Owns the host-side debug bookkeeping and the QMP/GDB stub lifecycle for one VM. QEMU's QMP socket is
    // the source of truth for the guest's run state. The factory arguments exist so tests can script both
    // transports without a VM.
    class DebugController
    {
    public:
        DebugController(std::filesystem::path qmpSocket,
                        std::filesystem::path stubSocket,
                        QmpTransportFactory   qmpFactory = {},
                        RspTransportFactory   rspFactory = {},
                        DebugInspector*       inspector  = nullptr);

        DebugController(const DebugController&)            = delete;
        DebugController& operator=(const DebugController&) = delete;

        // `busy` is set when another debug operation holds the controller; the caller maps it to Busy.
        // `unavailable` is set when QMP itself could not be reached, which the caller maps to
        // CapabilityUnavailable instead of Internal.
        [[nodiscard]] std::optional<DebugStatus> state(bool& busy, std::string& error, bool& unavailable);
        [[nodiscard]] std::optional<DebugStatus>
        control(DebugControlAction action, bool& busy, std::string& error, bool& unavailable);
        // `invalidState` is set when the guest is running or another stub owns the debug channel.
        [[nodiscard]] std::optional<std::vector<DebugRegisterRecord>> registers(const DebugRegistersRequest& request,
                                                                                bool&        invalidState,
                                                                                bool&        busy,
                                                                                std::string& error,
                                                                                bool&        unavailable);
        // Lists the stub's threads with their current context and, where the CR3 and stack resolve, the guest
        // process and thread. Reuses an attachment; otherwise starts and stops a temporary stub.
        [[nodiscard]] std::optional<std::vector<DebugThreadRecord>>   threads(
            const DebugThreadsRequest& request, bool& invalidState, bool& busy, std::string& error, bool& unavailable);
        // Runs one process-scoped write watchpoint to completion, a hit limit or a timeout. A start failure
        // returns nullopt; once the watch is armed, an error is reported through the end record instead.
        // `notFound` reports a module or symbol that could not be resolved, which the caller maps to NotFound.
        [[nodiscard]] std::optional<DebugWatchEnd> watch(DebugWatchRequest           request,
                                                         const DebugWatchHitSink&    onHit,
                                                         const DebugWatchAbortCheck& aborted,
                                                         bool&                       busy,
                                                         std::string&                error,
                                                         bool&                       unavailable,
                                                         bool&                       notFound);

        [[nodiscard]] std::optional<DebugWatchEnd> watch(DebugWatchRequest           request,
                                                         const DebugWatchHitSink&    onHit,
                                                         const DebugWatchAbortCheck& aborted,
                                                         bool&                       busy,
                                                         std::string&                error,
                                                         bool&                       unavailable)
        {
            bool notFound = false;
            return watch(std::move(request), onHit, aborted, busy, error, unavailable, notFound);
        }

        // Runs one breakpoint to its first hit or a timeout. Once the breakpoint is armed an error is
        // reported through the response; only setup failures return nullopt.
        [[nodiscard]] std::optional<DebugBreakpointResponse> breakpoint(DebugBreakpointRequest      request,
                                                                        const DebugWatchAbortCheck& aborted,
                                                                        bool&                       invalidState,
                                                                        bool&                       busy,
                                                                        std::string&                error,
                                                                        bool&                       unavailable,
                                                                        bool&                       notFound);

        [[nodiscard]] std::optional<DebugBreakpointResponse> breakpoint(DebugBreakpointRequest      request,
                                                                        const DebugWatchAbortCheck& aborted,
                                                                        bool&                       invalidState,
                                                                        bool&                       busy,
                                                                        std::string&                error,
                                                                        bool&                       unavailable)
        {
            bool notFound = false;
            return breakpoint(std::move(request), aborted, invalidState, busy, error, unavailable, notFound);
        }

        // Executes one instruction on a paused guest and returns the resulting registers.
        [[nodiscard]] std::optional<DebugStepResponse>
        step(const DebugStepRequest& request, bool& invalidState, bool& busy, std::string& error, bool& unavailable);

        // Starts one persistent stub owned by `sessionId` and pauses the guest if it is running. A second
        // session gets Busy; a foreign stub gets InvalidState and the debug-detach hint.
        [[nodiscard]] std::optional<DebugAttachResponse>
        attach(std::uint64_t sessionId, bool& invalidState, bool& busy, std::string& error, bool& unavailable);

        // Stops the attached stub or a stale foreign one, resumes the guest only when the attachment paused it,
        // and clears the bookkeeping.
        [[nodiscard]] std::optional<DebugDetachResponse>
        detach(std::uint64_t sessionId, bool& busy, std::string& error, bool& unavailable);

        // Best-effort detach when the owning session disappears. Never throws and never touches another
        // session's attachment.
        void detachSession(std::uint64_t sessionId) noexcept;

        // Persistent resources live on the attached stub. Every arm, remove and list call requires the
        // requesting session to own the attachment; a running guest is refused because QEMU cannot change a
        // debug register while it is running.
        [[nodiscard]] std::optional<DebugBreakpointArmResponse> armBreakpoint(std::uint64_t sessionId,
                                                                              const DebugBreakpointArmRequest& request,
                                                                              bool&        invalidState,
                                                                              bool&        busy,
                                                                              std::string& error,
                                                                              bool&        unavailable);
        [[nodiscard]] std::optional<DebugResourceRemoveResponse>
        removeBreakpoint(std::uint64_t                       sessionId,
                         const DebugBreakpointRemoveRequest& request,
                         bool&                               invalidState,
                         bool&                               busy,
                         std::string&                        error,
                         bool&                               unavailable);
        [[nodiscard]] std::optional<DebugBreakpointListResponse>
        listBreakpoints(std::uint64_t sessionId, bool& invalidState, bool& busy, std::string& error, bool& unavailable);

        [[nodiscard]] std::optional<DebugWatchArmResponse>       armWatch(std::uint64_t               sessionId,
                                                                          const DebugWatchArmRequest& request,
                                                                          bool&                       invalidState,
                                                                          bool&                       busy,
                                                                          std::string&                error,
                                                                          bool&                       unavailable);
        [[nodiscard]] std::optional<DebugResourceRemoveResponse> removeWatch(std::uint64_t                  sessionId,
                                                                             const DebugWatchRemoveRequest& request,
                                                                             bool&        invalidState,
                                                                             bool&        busy,
                                                                             std::string& error,
                                                                             bool&        unavailable);
        [[nodiscard]] std::optional<DebugWatchListResponse>
        listWatches(std::uint64_t sessionId, bool& invalidState, bool& busy, std::string& error, bool& unavailable);

        // Resumes the attached stub without removing any armed resource.
        [[nodiscard]] std::optional<DebugContinueResponse>
        continueGuest(std::uint64_t sessionId, bool& invalidState, bool& busy, std::string& error, bool& unavailable);

        // Waits for the next stop from any armed breakpoint or watchpoint. A currently stopped guest is resumed
        // first; the guest is left stopped at the last reported hit.
        [[nodiscard]] std::optional<DebugWaitEnd> wait(const DebugWaitRequest&     request,
                                                       const DebugWaitHitSink&     onHit,
                                                       const DebugWatchAbortCheck& aborted,
                                                       std::uint64_t               sessionId,
                                                       bool&                       invalidState,
                                                       bool&                       busy,
                                                       std::string&                error,
                                                       bool&                       unavailable);

        // Best-effort teardown for host shutdown: stops a stub this host owns and resumes a guest this host
        // paused. Never touches a foreign stub, and never throws.
        void shutdown() noexcept;

        void setInspector(DebugInspector* inspector) noexcept
        {
            _inspector = inspector;
        }

    private:
        using QmpCall = std::function<bool(QmpClient&, std::chrono::milliseconds, std::string&)>;

        struct ArmedResource
        {
            std::uint32_t       id {};
            DebugResourceKind   kind {DebugResourceKind::Breakpoint};
            DebugBreakpointKind breakpointKind {DebugBreakpointKind::Software};
            std::uint32_t       pid {};
            std::uint64_t       address {};
            std::uint32_t       width {};
            DebugWatchAccess    access {DebugWatchAccess::Write};
            std::uint32_t       slot {kNoDebugHardwareSlot};
            std::string         threadId; // the stub thread selected when the resource was armed
        };

        // All resource bookkeeping runs under _operationMutex.
        [[nodiscard]] bool          ownsAttachment(std::uint64_t sessionId, bool& invalidState, std::string& error);
        [[nodiscard]] bool          resolveRequestAddress(std::uint32_t      pid,
                                                          const std::string& module,
                                                          const std::string& symbol,
                                                          std::uint64_t      offset,
                                                          std::uint64_t&     address,
                                                          bool&              notFound,
                                                          std::string&       error);
        [[nodiscard]] bool          requirePausedForResource(bool& invalidState, std::string& error, bool& unavailable);
        [[nodiscard]] std::uint32_t allocateHardwareSlot() noexcept;
        void                        releaseHardwareSlot(std::uint32_t slot) noexcept;
        void                        refreshWatchCount() noexcept;
        [[nodiscard]] std::optional<DebugResourceRemoveResponse> removeResource(std::uint64_t     sessionId,
                                                                                std::uint32_t     id,
                                                                                DebugResourceKind kind,
                                                                                bool&             invalidState,
                                                                                std::string&      error,
                                                                                bool&             unavailable);
        // Removes every armed resource from the attached stub. The caller must have stopped the guest first.
        void                                                     clearArmedResources(RspClient* rsp) noexcept;

        [[nodiscard]] std::unique_ptr<QmpClient> makeQmp();
        [[nodiscard]] std::unique_ptr<RspClient> makeRsp();
        // Acquires the QMP mutex, reconnects when the cached channel is closed or absent, and runs one command
        // with the running request's remaining budget.
        [[nodiscard]] bool                       qmpCall(const QmpCall& call, std::string& error, bool& unavailable);
        [[nodiscard]] bool qmpQueryStatus(QmpStatusInfo& info, std::string& error, bool& unavailable);
        [[nodiscard]] bool qmpQueryCpusFast(QmpCpuInfo& info, std::string& error, bool& unavailable);
        // Rejects a guest that QMP already identifies as something other than x86-64. An empty architecture
        // means QMP did not say; the caller then checks the stub's target description after starting it.
        [[nodiscard]] bool checkQmpArchitecture(std::string& architecture, std::string& error, bool& unavailable);
        [[nodiscard]] bool
        qmpExecute(std::string_view command, std::string& result, std::string& error, bool& unavailable);
        [[nodiscard]] bool
        qmpHumanMonitor(std::string_view commandLine, std::string& output, std::string& error, bool& unavailable);
        [[nodiscard]] bool readStatus(DebugStatus& status, std::string& error, bool& unavailable);
        [[nodiscard]] bool hasActiveStub(bool& active, std::string& error, bool& unavailable);
        [[nodiscard]] bool stopStub(std::string& error, bool& unavailable);
        [[nodiscard]] bool startStub(RspClient& rsp, RspEndpoint& endpoint, std::string& error, bool& unavailable);
        [[nodiscard]] bool startUnixStub(RspClient& rsp, RspEndpoint& endpoint, std::string& error, bool& unavailable);
        [[nodiscard]] bool startTcpStub(RspClient& rsp, RspEndpoint& endpoint, std::string& error, bool& unavailable);
        [[nodiscard]] bool connectRsp(RspClient& rsp, const RspEndpoint& endpoint, std::string& error);
        [[nodiscard]] std::optional<std::vector<DebugRegisterRecord>>
                                                         readRecords(RspClient&                   rsp,
                                                                     const DebugRegistersRequest& request,
                                                                     std::string&                 architecture,
                                                                     std::string&                 error,
                                                                     bool&                        unavailable,
                                                                     const RspTargetDescription*  ready = nullptr);
        [[nodiscard]] std::optional<DebugRegisterRecord> readRecord(RspClient&                  rsp,
                                                                    const RspTargetDescription& description,
                                                                    std::uint32_t               vcpu,
                                                                    const std::string&          thread,
                                                                    std::string&                error);
        // Fills the guest pid/tid fields from the inspector. A missing process is normal and leaves the flags
        // clear; a lookup error is swallowed because hit reporting must not fail over symbolization.
        void                                             resolveGuestContext(std::uint64_t  cr3,
                                                                             std::uint64_t  rip,
                                                                             std::uint64_t  rsp,
                                                                             std::uint32_t& pid,
                                                                             std::uint32_t& tid,
                                                                             bool&          processFound,
                                                                             bool&          threadFound) noexcept;

        // Serializes the exclusive operations (registers, watch, breakpoint, step).
        std::mutex                 _operationMutex;
        // Serializes state() and control() so their bookkeeping cannot interleave.
        std::mutex                 _stateMutex;
        // Serializes QMP commands; held for one command, never for a whole watch or breakpoint run.
        std::mutex                 _qmpMutex;
        std::unique_ptr<QmpClient> _qmp;
        std::filesystem::path      _qmpSocket;
        std::filesystem::path      _stubSocket;
        QmpTransportFactory        _qmpFactory;
        RspTransportFactory        _rspFactory;
        DebugInspector*            _inspector {nullptr};
        std::unique_ptr<RspClient> _attachedRsp;
        RspEndpoint                _attachedEndpoint;
        RspTargetDescription       _attachedDescription;
        std::string                _attachedArchitecture;
        std::atomic<std::uint64_t> _ownerSession {0};
        bool                       _attachPaused {false};
        std::atomic<bool>          _stubActive {false};
        std::atomic<bool>          _hostPaused {false};
        std::atomic<bool>          _controlPaused {false};
        std::atomic<std::uint32_t> _watchCount {0};
        // True while the attached stub is running the guest after a continue or wait.
        std::atomic<bool>          _sessionRunning {false};
        std::vector<ArmedResource> _resources;
        std::uint32_t              _nextResourceId {1};
        std::uint32_t              _usedHardwareSlots {0};
    };
} // namespace mpfshost::debug

#endif // MPFSHOST_CORE_DEBUG_DEBUG_CONTROLLER_HPP
