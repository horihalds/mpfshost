#ifndef MPFSHOST_CORE_DEBUG_DEBUG_CONTROLLER_HPP
#define MPFSHOST_CORE_DEBUG_DEBUG_CONTROLLER_HPP

#pragma once

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
    using QmpTransportFactory = std::move_only_function<std::unique_ptr<QmpTransport>()>;
    using RspTransportFactory = std::move_only_function<std::unique_ptr<RspTransport>()>;

    // Owns the host-side debug bookkeeping and the QMP/GDB stub lifecycle for one VM. QEMU's QMP socket is
    // the source of truth for the guest's run state. The factory arguments exist so tests can script both
    // transports without a VM.
    class DebugController
    {
    public:
        DebugController(std::filesystem::path qmpSocket,
                        std::filesystem::path stubSocket,
                        QmpTransportFactory   qmpFactory = {},
                        RspTransportFactory   rspFactory = {});

        DebugController(const DebugController&)            = delete;
        DebugController& operator=(const DebugController&) = delete;

        [[nodiscard]] std::optional<DebugStatus> state(std::string& error);
        [[nodiscard]] std::optional<DebugStatus> control(DebugControlAction action, std::string& error);
        // `invalidState` is set when the guest is running or another stub owns the debug channel.
        [[nodiscard]] std::optional<std::vector<DebugRegisterRecord>>
        registers(const DebugRegistersRequest& request, bool& invalidState, std::string& error);

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

        std::mutex            _mutex;
        std::filesystem::path _qmpSocket;
        std::filesystem::path _stubSocket;
        QmpTransportFactory   _qmpFactory;
        RspTransportFactory   _rspFactory;
        bool                  _stubActive {false};
        bool                  _hostPaused {false};
        std::uint32_t         _watchCount {0};
    };
} // namespace mpfshost::debug

#endif // MPFSHOST_CORE_DEBUG_DEBUG_CONTROLLER_HPP
