#include "core/debug/debug_controller.hpp"

#include "core/log/log.hpp"
#include "core/server/process_info_provider.hpp"
#include "core/server/request_deadline.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <string_view>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mpfshost::debug
{
    namespace
    {
        constexpr auto kQmpTimeout     = std::chrono::seconds {5};
        constexpr auto kRspTimeout     = std::chrono::seconds {5};
        constexpr auto kRspConnectWait = std::chrono::seconds {2};

        bool looksLikeStubFailure(std::string_view reply)
        {
            return reply.find("couldn't") != std::string_view::npos || reply.find("duplicate") != std::string_view::npos
                || reply.find("error") != std::string_view::npos;
        }

        bool chardevHasGdb(std::string_view chardev)
        {
            std::size_t start = 0;
            while (start <= chardev.size())
            {
                const auto end = chardev.find('\n', start);
                auto line = chardev.substr(start, end == std::string_view::npos ? chardev.size() - start : end - start);
                if (!line.empty() && line.back() == '\r')
                {
                    line.remove_suffix(1);
                }
                while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                {
                    line.remove_prefix(1);
                }
                if (line.starts_with("gdb:"))
                {
                    return true;
                }
                if (end == std::string_view::npos)
                {
                    break;
                }
                start = end + 1;
            }
            return false;
        }

        std::uint16_t ephemeralLoopbackPort(std::string& error)
        {
            const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd < 0)
            {
                error = "could not reserve a loopback port";
                return 0;
            }

            sockaddr_in address {};
            address.sin_family      = AF_INET;
            address.sin_port        = 0;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
            {
                error = "could not reserve a loopback port";
                ::close(fd);
                return 0;
            }

            socklen_t length = sizeof(address);
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0)
            {
                error = "could not read the reserved loopback port";
                ::close(fd);
                return 0;
            }
            ::close(fd);
            return ntohs(address.sin_port);
        }

        std::optional<std::string> stopThreadId(std::string_view packet, bool& stop)
        {
            stop = !packet.empty() && (packet.front() == 'T' || packet.front() == 'S');
            if (!stop)
            {
                return std::nullopt;
            }
            const auto field = packet.find("thread:");
            if (field == std::string_view::npos)
            {
                return std::string {};
            }
            const auto start = field + 7;
            const auto end   = packet.find(';', start);
            return std::string {
                packet.substr(start, end == std::string_view::npos ? packet.size() - start : end - start)};
        }

        bool readNamedRegister(RspClient&                  rsp,
                               const RspTargetDescription& description,
                               std::string_view            name,
                               std::uint64_t&              value,
                               std::chrono::milliseconds   timeout,
                               std::string&                error)
        {
            if (const auto info = findRspRegister(description, name))
            {
                return rsp.readRegister(info->number, value, timeout, error);
            }
            if (const auto fallback = fallbackRspRegister(name))
            {
                return rsp.readRegister(fallback->number, value, timeout, error);
            }
            error = "the GDB stub does not expose " + std::string {name};
            return false;
        }

        bool readWatchValue(DebugInspector* inspector,
                            std::uint32_t   pid,
                            std::uint64_t   address,
                            std::uint32_t   width,
                            std::uint64_t&  value,
                            std::string&    error)
        {
            std::vector<std::byte> bytes;
            if (!inspector->readMemory(pid, address, width, bytes, error))
            {
                return false;
            }
            if (bytes.size() < width)
            {
                error = "the watched memory was shorter than the watch width";
                return false;
            }
            value = 0;
            for (std::uint32_t index = 0; index < width; ++index)
            {
                value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[index])) << (index * 8);
            }
            return true;
        }

        void resolveHitSymbols(DebugInspector* inspector, std::uint32_t pid, DebugWatchData& hit)
        {
            std::string error;
            const auto  module = inspector->moduleForAddress(pid, hit.rip, error);
            if (!module)
            {
                return;
            }
            hit.module       = module->name;
            hit.moduleOffset = module->offset;

            const auto exportHit = inspector->nearestExport(pid, module->name, hit.rip, error);
            if (exportHit.found)
            {
                hit.exportFound   = true;
                hit.exportAddress = exportHit.address;
                hit.exportOffset  = exportHit.offset;
                hit.exportName    = exportHit.name;
            }

            const auto symbol = inspector->pdbSymbol(pid, module->name, module->offset, error);
            if (symbol)
            {
                hit.pdbFound        = true;
                hit.pdbSymbol       = symbol->symbol;
                hit.pdbDisplacement = symbol->displacement;
            }
        }
    } // namespace

    DebugController::DebugController(std::filesystem::path qmpSocket,
                                     std::filesystem::path stubSocket,
                                     QmpTransportFactory   qmpFactory,
                                     RspTransportFactory   rspFactory,
                                     DebugInspector*       inspector)
        : _qmpSocket {std::move(qmpSocket)}, _stubSocket {std::move(stubSocket)}, _qmpFactory {std::move(qmpFactory)},
          _rspFactory {std::move(rspFactory)}, _inspector {inspector}
    {
    }

    std::unique_ptr<QmpClient> DebugController::makeQmp()
    {
        return std::make_unique<QmpClient>(_qmpFactory ? _qmpFactory() : makeUnixQmpTransport());
    }

    std::unique_ptr<RspClient> DebugController::makeRsp()
    {
        return std::make_unique<RspClient>(_rspFactory ? _rspFactory() : makeSocketRspTransport());
    }

    bool DebugController::connectQmp(std::unique_ptr<QmpClient>& client, std::string& error)
    {
        return client->connect(_qmpSocket, kQmpTimeout, error);
    }

    bool DebugController::readStatus(QmpClient& client, DebugStatus& status, std::string& error)
    {
        QmpStatusInfo info;
        if (!client.queryStatus(info, kQmpTimeout, error))
        {
            return false;
        }

        std::uint32_t vcpuCount = 0;
        if (!client.queryVcpuCount(vcpuCount, kQmpTimeout, error))
        {
            return false;
        }

        bool        active = false;
        std::string chardev;
        std::string ignored;
        if (client.humanMonitor("info chardev", chardev, kQmpTimeout, ignored))
        {
            active = chardevHasGdb(chardev);
        }

        status.runState   = qmpRunState(info.status);
        status.stubActive = _stubActive || active;
        status.hostPaused = _hostPaused;
        status.vcpuCount  = vcpuCount;
        status.watchCount = _watchCount;
        return true;
    }

    std::optional<DebugStatus> DebugController::state(bool& busy, std::string& error)
    {
        busy = false;
        std::unique_lock lock {_mutex, std::try_to_lock};
        if (!lock.owns_lock())
        {
            busy  = true;
            error = "another debug operation is in progress";
            return std::nullopt;
        }

        auto client = makeQmp();
        if (!connectQmp(client, error))
        {
            return std::nullopt;
        }

        DebugStatus status;
        if (!readStatus(*client, status, error))
        {
            return std::nullopt;
        }
        return status;
    }

    std::optional<DebugStatus> DebugController::control(DebugControlAction action, bool& busy, std::string& error)
    {
        busy = false;
        std::unique_lock lock {_mutex, std::try_to_lock};
        if (!lock.owns_lock())
        {
            busy  = true;
            error = "another debug operation is in progress";
            return std::nullopt;
        }

        auto client = makeQmp();
        if (!connectQmp(client, error))
        {
            return std::nullopt;
        }

        QmpStatusInfo current;
        if (!client->queryStatus(current, kQmpTimeout, error))
        {
            return std::nullopt;
        }

        const auto before = qmpRunState(current.status);
        if (action == DebugControlAction::Stop)
        {
            if (before == DebugRunState::Running)
            {
                std::string ignored;
                if (!client->execute("stop", ignored, kQmpTimeout, error))
                {
                    return std::nullopt;
                }
                _hostPaused = true;
                log::info("host", "paused the guest through QMP");
            }
        }
        else
        {
            if (before != DebugRunState::Running)
            {
                std::string ignored;
                if (!client->execute("cont", ignored, kQmpTimeout, error))
                {
                    return std::nullopt;
                }
                if (_hostPaused)
                {
                    log::info("host", "resumed the guest through QMP");
                }
            }
            _hostPaused = false;
        }

        DebugStatus status;
        if (!readStatus(*client, status, error))
        {
            return std::nullopt;
        }
        return status;
    }

    bool DebugController::hasActiveStub(QmpClient& client, bool& active, std::string& error)
    {
        std::string chardev;
        if (!client.humanMonitor("info chardev", chardev, kQmpTimeout, error))
        {
            return false;
        }
        active = chardevHasGdb(chardev);
        return true;
    }

    bool DebugController::stopStub(QmpClient& client, std::string& error)
    {
        if (!_stubActive)
        {
            return true;
        }
        std::string reply;
        if (!client.humanMonitor("gdbserver none", reply, kQmpTimeout, error))
        {
            return false;
        }
        _stubActive = false;
        log::info("host", "stopped the GDB stub");
        return true;
    }

    bool DebugController::connectRsp(RspClient& rsp, const RspEndpoint& endpoint, std::string& error)
    {
        const auto deadline = std::chrono::steady_clock::now() + kRspConnectWait;
        while (true)
        {
            if (rsp.connect(endpoint, std::chrono::milliseconds {250}, error))
            {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds {50});
        }
    }

    bool DebugController::startUnixStub(QmpClient& client, RspClient& rsp, RspEndpoint& endpoint, std::string& error)
    {
        std::error_code ignoredError;
        std::filesystem::remove(_stubSocket, ignoredError);

        std::string reply;
        if (!client.humanMonitor(
                "gdbserver unix:" + _stubSocket.string() + ",server=on,wait=off", reply, kQmpTimeout, error))
        {
            return false;
        }
        if (looksLikeStubFailure(reply))
        {
            error = reply.empty() ? "the Unix GDB stub could not be started" : reply;
            return false;
        }

        _stubActive = true;
        log::info("host", "started a Unix GDB stub at ", _stubSocket.string());

        endpoint.kind     = RspEndpoint::Kind::Unix;
        endpoint.unixPath = _stubSocket;
        return connectRsp(rsp, endpoint, error);
    }

    bool DebugController::startTcpStub(QmpClient& client, RspClient& rsp, RspEndpoint& endpoint, std::string& error)
    {
        const auto port = ephemeralLoopbackPort(error);
        if (port == 0)
        {
            return false;
        }

        const auto  address = "127.0.0.1:" + std::to_string(port);
        std::string reply;
        if (!client.humanMonitor("gdbserver tcp:" + address, reply, kQmpTimeout, error))
        {
            return false;
        }
        if (looksLikeStubFailure(reply))
        {
            error = reply.empty() ? "the TCP GDB stub could not be started" : reply;
            return false;
        }

        _stubActive = true;
        log::info("host", "started a loopback GDB stub at ", address);

        endpoint.kind    = RspEndpoint::Kind::Tcp;
        endpoint.tcpHost = "127.0.0.1";
        endpoint.tcpPort = port;
        return connectRsp(rsp, endpoint, error);
    }

    bool DebugController::startStub(QmpClient& client, RspClient& rsp, RspEndpoint& endpoint, std::string& error)
    {
        std::string unixError;
        if (startUnixStub(client, rsp, endpoint, unixError))
        {
            return true;
        }

        std::string ignored;
        if (_stubActive && !stopStub(client, ignored))
        {
            log::warn("host", "could not stop the Unix GDB stub: ", ignored);
        }
        std::error_code removeError;
        std::filesystem::remove(_stubSocket, removeError);

        std::string tcpError;
        if (startTcpStub(client, rsp, endpoint, tcpError))
        {
            return true;
        }
        if (_stubActive && !stopStub(client, ignored))
        {
            log::warn("host", "could not stop the loopback GDB stub: ", ignored);
        }

        error = "the GDB stub could not be started (unix: " + unixError + "; tcp: " + tcpError + ")";
        return false;
    }

    std::optional<std::vector<DebugRegisterRecord>>
    DebugController::readRecords(RspClient& rsp, const DebugRegistersRequest& request, std::string& error)
    {
        if (!rsp.negotiate(kRspTimeout, error))
        {
            return std::nullopt;
        }

        RspTargetDescription description;
        if (!rsp.readTargetDescription(description, kRspTimeout, error))
        {
            return std::nullopt;
        }

        std::vector<std::string> threads;
        if (!rsp.queryThreads(threads, kRspTimeout, error))
        {
            return std::nullopt;
        }
        if (threads.empty())
        {
            error = "the GDB stub reported no threads";
            return std::nullopt;
        }

        std::vector<std::size_t> selected;
        if (request.vcpu == kDebugAllVcpus)
        {
            selected.resize(threads.size());
            for (std::size_t index = 0; index < threads.size(); ++index)
            {
                selected[index] = index;
            }
        }
        else if (request.vcpu < threads.size())
        {
            selected.push_back(request.vcpu);
        }
        else
        {
            error = "vCPU " + std::to_string(request.vcpu) + " is not present";
            return std::nullopt;
        }

        std::vector<DebugRegisterRecord> records;
        records.reserve(selected.size());
        for (const auto index : selected)
        {
            if (requestExpired())
            {
                error = "the request deadline expired";
                return std::nullopt;
            }

            const auto& thread = threads[index];
            if (!rsp.selectThread(thread, kRspTimeout, error))
            {
                return std::nullopt;
            }

            std::vector<std::byte> raw;
            if (!rsp.readAllRegisters(raw, kRspTimeout, error))
            {
                return std::nullopt;
            }

            DebugRegisterRecord record;
            record.vcpu     = static_cast<std::uint32_t>(index);
            record.threadId = thread;

            const auto readRegister = [&](std::string_view name, std::uint64_t& value) -> bool
            {
                if (const auto info = findRspRegister(description, name))
                {
                    if (const auto found = readRspRegisterValue(raw, *info))
                    {
                        value = *found;
                        return true;
                    }
                }
                if (const auto fallback = fallbackRspRegister(name))
                {
                    return rsp.readRegister(fallback->number, value, kRspTimeout, error);
                }
                error = "the GDB stub does not expose " + std::string {name};
                return false;
            };

            const bool complete =
                readRegister("rip", record.rip) && readRegister("rsp", record.rsp) && readRegister("rbp", record.rbp)
                && readRegister("eflags", record.eflags) && readRegister("cr0", record.cr0)
                && readRegister("cr3", record.cr3) && readRegister("cr4", record.cr4) && readRegister("cr8", record.cr8)
                && readRegister("rax", record.rax) && readRegister("rbx", record.rbx) && readRegister("rcx", record.rcx)
                && readRegister("rdx", record.rdx) && readRegister("rsi", record.rsi) && readRegister("rdi", record.rdi)
                && readRegister("r8", record.r8) && readRegister("r9", record.r9) && readRegister("r10", record.r10)
                && readRegister("r11", record.r11) && readRegister("r12", record.r12) && readRegister("r13", record.r13)
                && readRegister("r14", record.r14) && readRegister("r15", record.r15);
            if (!complete)
            {
                return std::nullopt;
            }
            records.push_back(std::move(record));
        }
        return records;
    }

    std::optional<std::vector<DebugRegisterRecord>>
    DebugController::registers(const DebugRegistersRequest& request, bool& invalidState, bool& busy, std::string& error)
    {
        busy         = false;
        invalidState = false;
        std::unique_lock lock {_mutex, std::try_to_lock};
        if (!lock.owns_lock())
        {
            busy  = true;
            error = "another debug operation is in progress";
            return std::nullopt;
        }

        auto client = makeQmp();
        if (!connectQmp(client, error))
        {
            return std::nullopt;
        }

        QmpStatusInfo current;
        if (!client->queryStatus(current, kQmpTimeout, error))
        {
            return std::nullopt;
        }
        if (current.status == "running")
        {
            invalidState = true;
            error        = "the guest must be paused before its registers can be read";
            return std::nullopt;
        }

        if (_stubActive)
        {
            std::string ignored;
            if (!stopStub(*client, ignored))
            {
                error = ignored.empty() ? "the previous GDB stub could not be stopped" : ignored;
                return std::nullopt;
            }
        }

        bool active = false;
        if (!hasActiveStub(*client, active, error))
        {
            return std::nullopt;
        }
        if (active)
        {
            invalidState = true;
            error        = "another GDB stub owns the debug channel";
            return std::nullopt;
        }

        auto        rsp = makeRsp();
        RspEndpoint endpoint;
        if (!startStub(*client, *rsp, endpoint, error))
        {
            return std::nullopt;
        }

        struct Cleanup
        {
            DebugController& self;
            QmpClient&       client;

            ~Cleanup()
            {
                std::string ignored;
                if (self._stubActive && !self.stopStub(client, ignored))
                {
                    log::warn("host", "could not stop the GDB stub during teardown: ", ignored);
                }
                std::error_code removeError;
                std::filesystem::remove(self._stubSocket, removeError);

                QmpStatusInfo after;
                if (client.queryStatus(after, kQmpTimeout, ignored) && after.status == "running")
                {
                    std::string restore;
                    if (!client.execute("stop", restore, kQmpTimeout, ignored))
                    {
                        log::warn("host", "could not restore the paused guest after reading registers: ", ignored);
                    }
                }
            }
        } cleanup {*this, *client};

        return readRecords(*rsp, request, error);
    }

    std::optional<DebugWatchEnd> DebugController::watch(const DebugWatchRequest&    request,
                                                        const DebugWatchHitSink&    onHit,
                                                        const DebugWatchAbortCheck& aborted,
                                                        bool&                       busy,
                                                        std::string&                error)
    {
        busy = false;
        std::unique_lock lock {_mutex, std::try_to_lock};
        if (!lock.owns_lock())
        {
            busy  = true;
            error = "another debug operation is in progress";
            return std::nullopt;
        }
        if (_inspector == nullptr)
        {
            error = "the watchpoint service is unavailable";
            return std::nullopt;
        }

        auto client = makeQmp();
        if (!connectQmp(client, error))
        {
            return std::nullopt;
        }

        QmpStatusInfo current;
        if (!client->queryStatus(current, kQmpTimeout, error))
        {
            return std::nullopt;
        }

        bool stoppedByUs = false;
        if (current.status == "running")
        {
            std::string ignored;
            if (!client->execute("stop", ignored, kQmpTimeout, error))
            {
                return std::nullopt;
            }
            _hostPaused = true;
            stoppedByUs = true;
            log::info("host", "paused the guest for a watchpoint");
        }

        if (_stubActive)
        {
            std::string ignored;
            if (!stopStub(*client, ignored))
            {
                error = ignored.empty() ? "the previous GDB stub could not be stopped" : ignored;
                return std::nullopt;
            }
        }

        bool active = false;
        if (!hasActiveStub(*client, active, error))
        {
            return std::nullopt;
        }
        if (active)
        {
            error = "another GDB stub owns the debug channel";
            return std::nullopt;
        }

        auto        rsp = makeRsp();
        RspEndpoint endpoint;
        if (!startStub(*client, *rsp, endpoint, error))
        {
            return std::nullopt;
        }

        // QEMU's gdbstub keeps one global debug-register set: a second Z2 for the same address on another vCPU
        // is rejected, so the watchpoint is armed once rather than once per thread.
        std::optional<std::string> armedThread;

        struct Cleanup
        {
            DebugController&            self;
            QmpClient&                  client;
            RspClient&                  rsp;
            std::optional<std::string>& armedThread;
            const DebugWatchRequest&    request;
            bool                        stoppedByUs;

            ~Cleanup()
            {
                std::string ignored;
                rsp.drainPending();
                if (armedThread)
                {
                    if (rsp.selectThread(*armedThread, kRspTimeout, ignored)
                        && !rsp.removeWatchpoint(request.address, request.width, kRspTimeout, ignored))
                    {
                        log::warn("host", "could not remove the watchpoint: ", ignored);
                    }
                }
                self._watchCount = 0;

                // Detach the RSP client before stopping the stub; otherwise QEMU can stay in its gdb stop
                // state and the QMP continue below has no effect.
                rsp.disconnect();

                if (self._stubActive && !self.stopStub(client, ignored))
                {
                    log::warn("host", "could not stop the GDB stub during watchpoint teardown: ", ignored);
                }
                std::error_code removeError;
                std::filesystem::remove(self._stubSocket, removeError);

                QmpStatusInfo after;
                if (stoppedByUs)
                {
                    if (client.queryStatus(after, kQmpTimeout, ignored) && after.status != "running")
                    {
                        if (!client.execute("cont", ignored, kQmpTimeout, ignored))
                        {
                            log::warn("host", "could not resume the guest after a watchpoint: ", ignored);
                        }
                    }
                    QmpStatusInfo verified;
                    if (client.queryStatus(verified, kQmpTimeout, ignored) && verified.status != "running")
                    {
                        log::warn("host", "the guest is still not running after a watchpoint");
                    }
                    self._hostPaused = false;
                }
                else if (client.queryStatus(after, kQmpTimeout, ignored) && after.status == "running")
                {
                    if (!client.execute("stop", ignored, kQmpTimeout, ignored))
                    {
                        log::warn("host", "could not restore the paused guest after a watchpoint: ", ignored);
                    }
                }
            }
        } cleanup {*this, *client, *rsp, armedThread, request, stoppedByUs};

        if (!rsp->negotiate(kRspTimeout, error))
        {
            return std::nullopt;
        }
        RspTargetDescription description;
        if (!rsp->readTargetDescription(description, kRspTimeout, error))
        {
            return std::nullopt;
        }
        std::vector<std::string> threads;
        if (!rsp->queryThreads(threads, kRspTimeout, error))
        {
            return std::nullopt;
        }
        if (threads.empty())
        {
            error = "the GDB stub reported no threads";
            return std::nullopt;
        }

        const auto dtbs = _inspector->processDtbs(request.pid, error);
        if (!dtbs)
        {
            if (error.empty())
            {
                error = "process " + std::to_string(request.pid) + " is not available";
            }
            return std::nullopt;
        }

        std::uint64_t lastValue = 0;
        {
            std::string readError;
            if (!readWatchValue(_inspector, request.pid, request.address, request.width, lastValue, readError))
            {
                log::debug("host", "the initial watch value is unreadable: ", readError);
                lastValue = 0;
            }
        }

        if (!rsp->selectThread(threads.front(), kRspTimeout, error))
        {
            return std::nullopt;
        }
        if (!rsp->insertWatchpoint(request.address, request.width, kRspTimeout, error))
        {
            return std::nullopt;
        }
        armedThread = threads.front();
        _watchCount = 1;
        log::info("host", "armed a write watchpoint");

        if (!rsp->continueExecution(kRspTimeout, error))
        {
            return std::nullopt;
        }

        DebugWatchEnd end;
        end.status                  = DebugWatchStatus::Completed;
        std::uint32_t hits          = 0;
        const auto    watchDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds {request.timeoutMs};

        while (hits < request.maxHits)
        {
            if (aborted && aborted())
            {
                end.status = DebugWatchStatus::Cancelled;
                end.error  = "the watchpoint was cancelled";
                break;
            }
            if (requestExpired())
            {
                end.status = DebugWatchStatus::Timeout;
                end.error  = "the request deadline expired";
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= watchDeadline)
            {
                end.status = DebugWatchStatus::Timeout;
                end.error  = "the watchpoint timed out";
                break;
            }
            const auto slice = std::min(std::chrono::duration_cast<std::chrono::milliseconds>(watchDeadline - now),
                                        std::chrono::milliseconds {100});

            std::string stopPacket;
            const auto  polled = rsp->pollPacket(stopPacket, slice, error);
            if (polled == RspPollResult::Timeout)
            {
                continue;
            }
            if (polled == RspPollResult::Closed)
            {
                end.status = DebugWatchStatus::Error;
                end.error  = error.empty() ? "the GDB stub closed the connection" : error;
                break;
            }
            if (!stopPacket.empty() && (stopPacket.front() == 'W' || stopPacket.front() == 'X'))
            {
                end.status = DebugWatchStatus::Error;
                end.error  = "the guest stopped before the watchpoint fired";
                break;
            }

            bool       stopped = false;
            const auto thread  = stopThreadId(stopPacket, stopped);
            if (!stopped)
            {
                continue;
            }
            if (thread && !thread->empty() && !rsp->selectThread(*thread, kRspTimeout, error))
            {
                end.status = DebugWatchStatus::Error;
                end.error  = error;
                break;
            }

            std::uint64_t rip      = 0;
            std::uint64_t rspValue = 0;
            std::uint64_t cr3      = 0;
            if (!readNamedRegister(*rsp, description, "rip", rip, kRspTimeout, error)
                || !readNamedRegister(*rsp, description, "rsp", rspValue, kRspTimeout, error)
                || !readNamedRegister(*rsp, description, "cr3", cr3, kRspTimeout, error))
            {
                end.status = DebugWatchStatus::Error;
                end.error  = error;
                break;
            }

            const auto hitDtb = canonicalDtb(cr3);
            if (hitDtb != canonicalDtb(dtbs->dtb) && hitDtb != canonicalDtb(dtbs->dtbUser))
            {
                ++end.skippedCount;
                if (!rsp->continueExecution(kRspTimeout, error))
                {
                    end.status = DebugWatchStatus::Error;
                    end.error  = error;
                    break;
                }
                continue;
            }

            std::uint64_t newValue = 0;
            if (!readWatchValue(_inspector, request.pid, request.address, request.width, newValue, error))
            {
                end.status = DebugWatchStatus::Error;
                end.error  = error;
                break;
            }

            DebugWatchData hit;
            hit.hitIndex = hits;
            hit.vcpu     = 0;
            hit.threadId = thread ? *thread : std::string {};
            if (!hit.threadId.empty())
            {
                const auto found = std::find(threads.begin(), threads.end(), hit.threadId);
                if (found != threads.end())
                {
                    hit.vcpu = static_cast<std::uint32_t>(std::distance(threads.begin(), found));
                }
            }
            hit.rip      = rip;
            hit.rsp      = rspValue;
            hit.cr3      = cr3;
            hit.address  = request.address;
            hit.oldValue = lastValue;
            hit.newValue = newValue;
            lastValue    = newValue;

            const auto beforeStart = rip > kDebugWatchBytesBefore ? rip - kDebugWatchBytesBefore : 0;
            const auto beforeSize  = static_cast<std::uint32_t>(rip - beforeStart);
            if (beforeSize > 0)
            {
                std::string readError;
                if (!_inspector->readMemory(request.pid, beforeStart, beforeSize, hit.bytesBefore, readError))
                {
                    hit.bytesBefore.clear();
                }
            }

            resolveHitSymbols(_inspector, request.pid, hit);
            ++hits;
            end.hitCount = hits;

            if (onHit && !onHit(hit))
            {
                end.status = DebugWatchStatus::Cancelled;
                end.error  = "the watchpoint result could not be delivered";
                break;
            }
            if (hits >= request.maxHits)
            {
                break;
            }
            if (!rsp->continueExecution(kRspTimeout, error))
            {
                end.status = DebugWatchStatus::Error;
                end.error  = error;
                break;
            }
        }

        end.hitCount = hits;
        return end;
    }
} // namespace mpfshost::debug
