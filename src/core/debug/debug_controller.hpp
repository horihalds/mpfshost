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
#include <vector>

namespace mpfshost::debug
{
    using QmpTransportFactory  = std::move_only_function<std::unique_ptr<QmpTransport>()>;
    using RspTransportFactory  = std::move_only_function<std::unique_ptr<RspTransport>()>;
    using DebugWatchHitSink    = std::function<bool(const DebugWatchData&)>;
    using DebugWatchAbortCheck = std::function<bool()>;

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
        // Runs one process-scoped write watchpoint to completion, a hit limit or a timeout. A start failure
        // returns nullopt; once the watch is armed, an error is reported through the end record instead.
        [[nodiscard]] std::optional<DebugWatchEnd>                    watch(const DebugWatchRequest&    request,
                                                                            const DebugWatchHitSink&    onHit,
                                                                            const DebugWatchAbortCheck& aborted,
                                                                            bool&                       busy,
                                                                            std::string&                error,
                                                                            bool&                       unavailable);
        // Runs one breakpoint to its first hit or a timeout. Once the breakpoint is armed an error is
        // reported through the response; only setup failures return nullopt.
        [[nodiscard]] std::optional<DebugBreakpointResponse>          breakpoint(const DebugBreakpointRequest& request,
                                                                                 const DebugWatchAbortCheck&   aborted,
                                                                                 bool&                         invalidState,
                                                                                 bool&                         busy,
                                                                                 std::string&                  error,
                                                                                 bool&                         unavailable);
        // Executes one instruction on a paused guest and returns the resulting registers.
        [[nodiscard]] std::optional<DebugStepResponse>
        step(const DebugStepRequest& request, bool& invalidState, bool& busy, std::string& error, bool& unavailable);

        // Stops an active GDB stub, including one this host did not start, and resumes the guest only when this
        // host paused it. Used to recover a target left with a stale stub after a host restart.
        [[nodiscard]] std::optional<DebugDetachResponse> detach(bool& busy, std::string& error, bool& unavailable);

        // Best-effort teardown for host shutdown: stops a stub this host owns and resumes a guest this host
        // paused. Never touches a foreign stub, and never throws.
        void shutdown() noexcept;

        void setInspector(DebugInspector* inspector) noexcept
        {
            _inspector = inspector;
        }

    private:
        using QmpCall = std::function<bool(QmpClient&, std::chrono::milliseconds, std::string&)>;

        [[nodiscard]] std::unique_ptr<QmpClient> makeQmp();
        [[nodiscard]] std::unique_ptr<RspClient> makeRsp();
        // Acquires the QMP mutex, reconnects when the cached channel is closed or absent, and runs one command
        // with the running request's remaining budget.
        [[nodiscard]] bool                       qmpCall(const QmpCall& call, std::string& error, bool& unavailable);
        [[nodiscard]] bool qmpQueryStatus(QmpStatusInfo& info, std::string& error, bool& unavailable);
        [[nodiscard]] bool qmpQueryVcpuCount(std::uint32_t& count, std::string& error, bool& unavailable);
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
        readRecords(RspClient& rsp, const DebugRegistersRequest& request, std::string& error);
        [[nodiscard]] std::optional<DebugRegisterRecord> readRecord(RspClient&                  rsp,
                                                                    const RspTargetDescription& description,
                                                                    std::uint32_t               vcpu,
                                                                    const std::string&          thread,
                                                                    std::string&                error);

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
        std::atomic<bool>          _stubActive {false};
        std::atomic<bool>          _hostPaused {false};
        std::atomic<bool>          _controlPaused {false};
        std::atomic<std::uint32_t> _watchCount {0};
    };
} // namespace mpfshost::debug

#endif // MPFSHOST_CORE_DEBUG_DEBUG_CONTROLLER_HPP
