#ifndef MPFSHOST_CORE_DEBUG_DEBUG_CONTROLLER_HPP
#define MPFSHOST_CORE_DEBUG_DEBUG_CONTROLLER_HPP

#pragma once

#include "core/debug/debug_inspector.hpp"
#include "core/debug/qmp_client.hpp"
#include "core/debug/rsp_client.hpp"
#include "core/ipc/message.hpp"

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
        [[nodiscard]] std::optional<DebugStatus> state(bool& busy, std::string& error);
        [[nodiscard]] std::optional<DebugStatus> control(DebugControlAction action, bool& busy, std::string& error);
        // `invalidState` is set when the guest is running or another stub owns the debug channel.
        [[nodiscard]] std::optional<std::vector<DebugRegisterRecord>>
        registers(const DebugRegistersRequest& request, bool& invalidState, bool& busy, std::string& error);
        // Runs one process-scoped write watchpoint to completion, a hit limit or a timeout. A start failure
        // returns nullopt; once the watch is armed, an error is reported through the end record instead.
        [[nodiscard]] std::optional<DebugWatchEnd>           watch(const DebugWatchRequest&    request,
                                                                   const DebugWatchHitSink&    onHit,
                                                                   const DebugWatchAbortCheck& aborted,
                                                                   bool&                       busy,
                                                                   std::string&                error);
        // Runs one breakpoint to its first hit or a timeout. Once the breakpoint is armed an error is
        // reported through the response; only setup failures return nullopt.
        [[nodiscard]] std::optional<DebugBreakpointResponse> breakpoint(const DebugBreakpointRequest& request,
                                                                        const DebugWatchAbortCheck&   aborted,
                                                                        bool&                         invalidState,
                                                                        bool&                         busy,
                                                                        std::string&                  error);
        // Executes one instruction on a paused guest and returns the resulting registers.
        [[nodiscard]] std::optional<DebugStepResponse>
        step(const DebugStepRequest& request, bool& invalidState, bool& busy, std::string& error);

        // Stops an active GDB stub, including one this host did not start, and resumes the guest only when this
        // host paused it. Used to recover a target left with a stale stub after a host restart.
        [[nodiscard]] std::optional<DebugDetachResponse> detach(bool& busy, std::string& error);

        // Best-effort teardown for host shutdown: stops a stub this host owns and resumes a guest this host
        // paused. Never touches a foreign stub, and never throws.
        void shutdown() noexcept;

        void setInspector(DebugInspector* inspector) noexcept
        {
            _inspector = inspector;
        }

    private:
        [[nodiscard]] std::unique_ptr<QmpClient> makeQmp();
        [[nodiscard]] std::unique_ptr<RspClient> makeRsp();
        [[nodiscard]] bool                       connectQmp(std::unique_ptr<QmpClient>& client, std::string& error);
        [[nodiscard]] bool                       readStatus(QmpClient& client, DebugStatus& status, std::string& error);
        [[nodiscard]] bool                       hasActiveStub(QmpClient& client, bool& active, std::string& error);
        [[nodiscard]] bool                       stopStub(QmpClient& client, std::string& error);
        [[nodiscard]] bool startStub(QmpClient& client, RspClient& rsp, RspEndpoint& endpoint, std::string& error);
        [[nodiscard]] bool startUnixStub(QmpClient& client, RspClient& rsp, RspEndpoint& endpoint, std::string& error);
        [[nodiscard]] bool startTcpStub(QmpClient& client, RspClient& rsp, RspEndpoint& endpoint, std::string& error);
        [[nodiscard]] bool connectRsp(RspClient& rsp, const RspEndpoint& endpoint, std::string& error);
        [[nodiscard]] std::optional<std::vector<DebugRegisterRecord>>
        readRecords(RspClient& rsp, const DebugRegistersRequest& request, std::string& error);
        [[nodiscard]] std::optional<DebugRegisterRecord> readRecord(RspClient&                  rsp,
                                                                    const RspTargetDescription& description,
                                                                    std::uint32_t               vcpu,
                                                                    const std::string&          thread,
                                                                    std::string&                error);

        std::mutex            _mutex;
        std::filesystem::path _qmpSocket;
        std::filesystem::path _stubSocket;
        QmpTransportFactory   _qmpFactory;
        RspTransportFactory   _rspFactory;
        DebugInspector*       _inspector {nullptr};
        bool                  _stubActive {false};
        bool                  _hostPaused {false};
        std::uint32_t         _watchCount {0};
    };
} // namespace mpfshost::debug

#endif // MPFSHOST_CORE_DEBUG_DEBUG_CONTROLLER_HPP
