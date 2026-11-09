#include "core/debug/debug_controller.hpp"

#include "core/log/log.hpp"
#include "core/server/process_info_provider.hpp"
#include "core/server/request_deadline.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <optional>
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

        // The register layout, rip/cr3 lookups and the x86 decoder only understand x86-64.
        constexpr std::string_view kSupportedArchitecture = "x86_64";

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

        // QEMU names the watched linear address in a write-watchpoint stop. It is the only positive evidence
        // that the stop belongs to the armed watchpoint, independent of the process DTB and module maps.
        std::optional<std::uint64_t> stopWatchAddress(std::string_view packet)
        {
            if (packet.empty() || packet.front() != 'T')
            {
                return std::nullopt;
            }
            const auto field = packet.find("watch:");
            if (field == std::string_view::npos)
            {
                return std::nullopt;
            }
            const auto start = field + 6;
            const auto end   = packet.find(';', start);
            const auto text = packet.substr(start, end == std::string_view::npos ? packet.size() - start : end - start);
            if (text.empty())
            {
                return std::nullopt;
            }

            std::uint64_t address = 0;
            const auto    result  = std::from_chars(text.data(), text.data() + text.size(), address, 16);
            if (result.ec != std::errc {} || result.ptr != text.data() + text.size())
            {
                return std::nullopt;
            }
            return address;
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

        // An x86-64 canonical address below the halfway point belongs to the user half of the address space.
        bool isUserModeAddress(std::uint64_t address) noexcept
        {
            return address < (1ull << 47);
        }

        enum class ThreadSelection
        {
            Found,
            NotInProcess,
            Error,
        };

        // A software breakpoint patches the selected vCPU's address space, so arming a user-mode address from a
        // vCPU in another process makes QEMU reject the write. Pick a vCPU that is already in the target process.
        ThreadSelection selectThreadInProcess(RspClient&                      rsp,
                                              const RspTargetDescription&     description,
                                              const std::vector<std::string>& threads,
                                              std::uint64_t                   dtb,
                                              std::uint64_t                   dtbUser,
                                              std::string&                    chosen,
                                              std::string&                    error)
        {
            for (const auto& thread : threads)
            {
                if (!rsp.selectThread(thread, kRspTimeout, error))
                {
                    return ThreadSelection::Error;
                }
                std::uint64_t cr3 = 0;
                if (!readNamedRegister(rsp, description, "cr3", cr3, kRspTimeout, error))
                {
                    return ThreadSelection::Error;
                }
                const auto candidate = canonicalDtb(cr3);
                if (candidate == canonicalDtb(dtb) || candidate == canonicalDtb(dtbUser))
                {
                    chosen = thread;
                    return ThreadSelection::Found;
                }
            }
            error = "no vCPU is executing in the target process; a software breakpoint on a user-mode address "
                    "cannot be armed from another address space (use a hardware breakpoint instead)";
            return ThreadSelection::NotInProcess;
        }

        bool isSupportedArchitecture(std::string_view architecture)
        {
            return architecture == kSupportedArchitecture;
        }

        // The target description is the fallback when QMP's query-cpus-fast does not name the architecture.
        // Register names are enough to tell the families apart before any register is read.
        std::string architectureFromTargetDescription(const RspTargetDescription& description)
        {
            if (findRspRegister(description, "rip"))
            {
                return "x86_64";
            }
            if (findRspRegister(description, "eip"))
            {
                return "i386";
            }
            if (findRspRegister(description, "pc"))
            {
                if (findRspRegister(description, "x0"))
                {
                    return "aarch64";
                }
                return "arm";
            }
            return {};
        }

        bool rejectArchitecture(std::string_view architecture, bool fromStub, std::string& error, bool& unavailable)
        {
            unavailable = true;
            if (architecture.empty())
            {
                error = fromStub
                          ? "the GDB target description does not identify a supported architecture; only x86_64 guests "
                            "can be debugged"
                          : "the guest architecture is unknown; only x86_64 guests can be debugged";
                return false;
            }
            error = "the guest architecture '" + std::string {architecture}
                  + "' is not supported; only x86_64 guests can be debugged";
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

        struct DebugLocation
        {
            std::string   module;
            std::uint64_t moduleOffset {};
            bool          exportFound {};
            std::uint64_t exportAddress {};
            std::uint64_t exportOffset {};
            std::string   exportName;
            bool          pdbFound {};
            std::string   pdbSymbol;
            std::uint32_t pdbDisplacement {};
        };

        void
        resolveLocation(DebugInspector* inspector, std::uint32_t pid, std::uint64_t address, DebugLocation& location)
        {
            std::string error;
            const auto  module = inspector->moduleForAddress(pid, address, error);
            if (!module)
            {
                return;
            }
            location.module       = module->name;
            location.moduleOffset = module->offset;

            const auto exportHit = inspector->nearestExport(pid, module->name, address, error);
            if (exportHit.found)
            {
                location.exportFound   = true;
                location.exportAddress = exportHit.address;
                location.exportOffset  = exportHit.offset;
                location.exportName    = exportHit.name;
            }

            const auto symbol = inspector->pdbSymbol(pid, module->name, module->offset, error);
            if (symbol)
            {
                location.pdbFound        = true;
                location.pdbSymbol       = symbol->symbol;
                location.pdbDisplacement = symbol->displacement;
            }
        }

        void resolveHitSymbols(DebugInspector* inspector, std::uint32_t pid, DebugWatchData& hit)
        {
            DebugLocation location;
            resolveLocation(inspector, pid, hit.rip, location);
            hit.module          = std::move(location.module);
            hit.moduleOffset    = location.moduleOffset;
            hit.exportFound     = location.exportFound;
            hit.exportAddress   = location.exportAddress;
            hit.exportOffset    = location.exportOffset;
            hit.exportName      = std::move(location.exportName);
            hit.pdbFound        = location.pdbFound;
            hit.pdbSymbol       = std::move(location.pdbSymbol);
            hit.pdbDisplacement = location.pdbDisplacement;
        }

        void resolveWaitHitSymbols(DebugInspector* inspector, std::uint32_t pid, DebugWaitHit& hit)
        {
            DebugLocation location;
            resolveLocation(inspector, pid, hit.record.rip, location);
            hit.module          = std::move(location.module);
            hit.moduleOffset    = location.moduleOffset;
            hit.exportFound     = location.exportFound;
            hit.exportAddress   = location.exportAddress;
            hit.exportOffset    = location.exportOffset;
            hit.exportName      = std::move(location.exportName);
            hit.pdbFound        = location.pdbFound;
            hit.pdbSymbol       = std::move(location.pdbSymbol);
            hit.pdbDisplacement = location.pdbDisplacement;
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

    bool DebugController::qmpCall(const QmpCall& call, std::string& error, bool& unavailable)
    {
        std::lock_guard lock {_qmpMutex};
        auto            timeout = requestRemaining(kQmpTimeout);
        if (timeout <= std::chrono::milliseconds::zero())
        {
            // The request budget can expire exactly when a watch or breakpoint times out. Teardown still has to
            // stop the stub and restore the guest, so give those trailing QMP commands a bounded budget too.
            timeout = kQmpTimeout;
        }

        if (_qmp && !_qmp->isOpen())
        {
            _qmp.reset();
        }
        if (!_qmp)
        {
            auto client = makeQmp();
            if (!client->connect(_qmpSocket, timeout, error))
            {
                // QMP is not a command failure: the socket or its peer is gone, which is a capability issue.
                unavailable = true;
                if (error.empty())
                {
                    error = "the QMP socket is not available";
                }
                return false;
            }
            _qmp = std::move(client);
        }

        if (!call(*_qmp, timeout, error))
        {
            // A transport failure marks the channel closed; drop it so the next call reconnects. A protocol
            // error such as an unknown command leaves the channel usable.
            if (!_qmp->isOpen())
            {
                _qmp.reset();
            }
            return false;
        }
        return true;
    }

    bool DebugController::qmpQueryStatus(QmpStatusInfo& info, std::string& error, bool& unavailable)
    {
        return qmpCall(
            [&](QmpClient& client, std::chrono::milliseconds timeout, std::string& callError)
            {
                return client.queryStatus(info, timeout, callError);
            },
            error,
            unavailable);
    }

    bool DebugController::qmpQueryCpusFast(QmpCpuInfo& info, std::string& error, bool& unavailable)
    {
        return qmpCall(
            [&](QmpClient& client, std::chrono::milliseconds timeout, std::string& callError)
            {
                return client.queryCpusFast(info, timeout, callError);
            },
            error,
            unavailable);
    }

    bool DebugController::checkQmpArchitecture(std::string& architecture, std::string& error, bool& unavailable)
    {
        QmpCpuInfo info;
        if (!qmpQueryCpusFast(info, error, unavailable))
        {
            return false;
        }
        architecture = info.architecture;
        if (!architecture.empty() && !isSupportedArchitecture(architecture))
        {
            return rejectArchitecture(architecture, false, error, unavailable);
        }
        return true;
    }

    bool
    DebugController::qmpExecute(std::string_view command, std::string& result, std::string& error, bool& unavailable)
    {
        return qmpCall(
            [&](QmpClient& client, std::chrono::milliseconds timeout, std::string& callError)
            {
                return client.execute(command, result, timeout, callError);
            },
            error,
            unavailable);
    }

    bool DebugController::qmpHumanMonitor(std::string_view commandLine,
                                          std::string&     output,
                                          std::string&     error,
                                          bool&            unavailable)
    {
        return qmpCall(
            [&](QmpClient& client, std::chrono::milliseconds timeout, std::string& callError)
            {
                return client.humanMonitor(commandLine, output, timeout, callError);
            },
            error,
            unavailable);
    }

    bool DebugController::readStatus(DebugStatus& status, std::string& error, bool& unavailable)
    {
        QmpStatusInfo info;
        if (!qmpQueryStatus(info, error, unavailable))
        {
            return false;
        }

        QmpCpuInfo cpus;
        if (!qmpQueryCpusFast(cpus, error, unavailable))
        {
            return false;
        }

        bool        active = false;
        std::string chardev;
        std::string ignored;
        bool        chardevUnavailable = false;
        if (qmpHumanMonitor("info chardev", chardev, ignored, chardevUnavailable))
        {
            active = chardevHasGdb(chardev);
        }

        const bool stubActive = _stubActive.load();
        status.runState       = qmpRunState(info.status);
        status.stubActive     = stubActive || active;
        status.stubOwned      = stubActive;
        status.foreignStub    = active && !stubActive;
        status.hostPaused     = _hostPaused.load();
        status.vcpuCount      = cpus.count;
        status.watchCount     = _watchCount.load();
        status.watchAccess    = _watchAccess.load();
        status.architecture   = cpus.architecture.empty() ? "unknown" : cpus.architecture;
        return true;
    }

    std::optional<DebugStatus> DebugController::state(bool& busy, std::string& error, bool& unavailable)
    {
        busy        = false;
        unavailable = false;
        std::unique_lock lock {_stateMutex, std::try_to_lock};
        if (!lock.owns_lock())
        {
            busy  = true;
            error = "another state query or control request is in progress";
            return std::nullopt;
        }

        DebugStatus status;
        if (!readStatus(status, error, unavailable))
        {
            return std::nullopt;
        }
        return status;
    }

    std::optional<DebugStatus>
    DebugController::control(DebugControlAction action, bool& busy, std::string& error, bool& unavailable)
    {
        busy        = false;
        unavailable = false;
        std::unique_lock lock {_stateMutex, std::try_to_lock};
        if (!lock.owns_lock())
        {
            busy  = true;
            error = "another state query or control request is in progress";
            return std::nullopt;
        }

        QmpStatusInfo current;
        if (!qmpQueryStatus(current, error, unavailable))
        {
            return std::nullopt;
        }

        const auto before = qmpRunState(current.status);
        if (action == DebugControlAction::Stop)
        {
            if (before == DebugRunState::Running)
            {
                std::string ignored;
                if (!qmpExecute("stop", ignored, error, unavailable))
                {
                    return std::nullopt;
                }
                _hostPaused = true;
                log::info("host", "paused the guest through QMP");
            }
            _sessionRunning = false;
            // A watch in flight may see the resulting stub stop; it must not resume the guest underneath us.
            _controlPaused  = true;
        }
        else
        {
            _controlPaused = false;
            if (before != DebugRunState::Running)
            {
                std::string ignored;
                if (!qmpExecute("cont", ignored, error, unavailable))
                {
                    return std::nullopt;
                }
                if (_hostPaused.load())
                {
                    log::info("host", "resumed the guest through QMP");
                }
            }
            _hostPaused     = false;
            _sessionRunning = true;
        }

        DebugStatus status;
        if (!readStatus(status, error, unavailable))
        {
            return std::nullopt;
        }
        return status;
    }

    bool DebugController::hasActiveStub(bool& active, std::string& error, bool& unavailable)
    {
        std::string chardev;
        if (!qmpHumanMonitor("info chardev", chardev, error, unavailable))
        {
            return false;
        }
        active = chardevHasGdb(chardev);
        return true;
    }

    bool DebugController::stopStub(std::string& error, bool& unavailable)
    {
        if (!_stubActive.load())
        {
            return true;
        }
        std::string reply;
        if (!qmpHumanMonitor("gdbserver none", reply, error, unavailable))
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

    bool DebugController::startUnixStub(RspClient& rsp, RspEndpoint& endpoint, std::string& error, bool& unavailable)
    {
        std::error_code ignoredError;
        std::filesystem::remove(_stubSocket, ignoredError);

        std::string reply;
        if (!qmpHumanMonitor(
                "gdbserver unix:" + _stubSocket.string() + ",server=on,wait=off", reply, error, unavailable))
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

    bool DebugController::startTcpStub(RspClient& rsp, RspEndpoint& endpoint, std::string& error, bool& unavailable)
    {
        const auto port = ephemeralLoopbackPort(error);
        if (port == 0)
        {
            return false;
        }

        const auto  address = "127.0.0.1:" + std::to_string(port);
        std::string reply;
        if (!qmpHumanMonitor("gdbserver tcp:" + address, reply, error, unavailable))
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

    bool DebugController::startStub(RspClient& rsp, RspEndpoint& endpoint, std::string& error, bool& unavailable)
    {
        std::string unixError;
        bool        unixUnavailable = false;
        if (startUnixStub(rsp, endpoint, unixError, unixUnavailable))
        {
            return true;
        }

        std::string ignored;
        bool        ignoredUnavailable = false;
        if (_stubActive.load() && !stopStub(ignored, ignoredUnavailable))
        {
            log::warn("host", "could not stop the Unix GDB stub: ", ignored);
        }
        std::error_code removeError;
        std::filesystem::remove(_stubSocket, removeError);

        std::string tcpError;
        bool        tcpUnavailable = false;
        if (startTcpStub(rsp, endpoint, tcpError, tcpUnavailable))
        {
            return true;
        }
        if (_stubActive.load() && !stopStub(ignored, ignoredUnavailable))
        {
            log::warn("host", "could not stop the loopback GDB stub: ", ignored);
        }

        unavailable = unixUnavailable || tcpUnavailable;
        error       = "the GDB stub could not be started (unix: " + unixError + "; tcp: " + tcpError + ")";
        return false;
    }

    std::optional<std::vector<DebugRegisterRecord>> DebugController::readRecords(RspClient&                   rsp,
                                                                                 const DebugRegistersRequest& request,
                                                                                 std::string& architecture,
                                                                                 std::string& error,
                                                                                 bool&        unavailable,
                                                                                 const RspTargetDescription* ready)
    {
        RspTargetDescription        localDescription;
        const RspTargetDescription* description = ready;
        if (description == nullptr)
        {
            if (!rsp.negotiate(kRspTimeout, error))
            {
                return std::nullopt;
            }

            if (!rsp.readTargetDescription(localDescription, kRspTimeout, error))
            {
                return std::nullopt;
            }
            if (architecture.empty())
            {
                architecture = architectureFromTargetDescription(localDescription);
                if (!isSupportedArchitecture(architecture))
                {
                    rejectArchitecture(architecture, true, error, unavailable);
                    return std::nullopt;
                }
            }
            description = &localDescription;
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

            auto record = readRecord(rsp, *description, static_cast<std::uint32_t>(index), thread, error);
            if (!record)
            {
                return std::nullopt;
            }
            records.push_back(std::move(*record));
        }
        return records;
    }

    std::optional<DebugRegisterRecord> DebugController::readRecord(RspClient&                  rsp,
                                                                   const RspTargetDescription& description,
                                                                   std::uint32_t               vcpu,
                                                                   const std::string&          thread,
                                                                   std::string&                error)
    {
        std::vector<std::byte> raw;
        if (!rsp.readAllRegisters(raw, kRspTimeout, error))
        {
            return std::nullopt;
        }

        DebugRegisterRecord record;
        record.vcpu     = vcpu;
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
        resolveGuestContext(record.cr3,
                            record.rip,
                            record.rsp,
                            record.guestPid,
                            record.guestTid,
                            record.processFound,
                            record.threadFound);
        return record;
    }

    void DebugController::resolveGuestContext(std::uint64_t  cr3,
                                              std::uint64_t  rip,
                                              std::uint64_t  rsp,
                                              std::uint32_t& pid,
                                              std::uint32_t& tid,
                                              bool&          processFound,
                                              bool&          threadFound) noexcept
    {
        pid          = 0;
        tid          = 0;
        processFound = false;
        threadFound  = false;
        if (_inspector == nullptr)
        {
            return;
        }

        std::string ignored;
        const auto  guest = _inspector->guestThread(cr3, rip, rsp, ignored);
        if (!guest)
        {
            return;
        }
        pid          = guest->pid;
        tid          = guest->tid;
        processFound = true;
        threadFound  = guest->threadFound;
    }

    bool DebugController::resolveRequestAddress(std::uint32_t      pid,
                                                const std::string& module,
                                                const std::string& symbol,
                                                std::uint64_t      offset,
                                                std::uint64_t&     address,
                                                bool&              notFound,
                                                std::string&       error)
    {
        notFound = false;
        address  = offset;
        if (module.empty())
        {
            return true;
        }
        if (_inspector == nullptr)
        {
            error = "the symbol service is unavailable";
            return false;
        }

        error.clear();
        const auto hit = _inspector->resolveSymbol(pid, module, symbol, error);
        if (!hit.found)
        {
            notFound = true;
            if (error.empty())
            {
                error = hit.ambiguous ? "module '" + module + "' matches more than one loaded image"
                                      : "symbol '" + symbol + "' was not found in module '" + module + "'";
            }
            return false;
        }
        if (offset > std::numeric_limits<std::uint64_t>::max() - hit.address)
        {
            error = "the symbolic address overflows a 64-bit virtual address";
            return false;
        }
        address = hit.address + offset;
        return true;
    }

    std::optional<std::vector<DebugRegisterRecord>> DebugController::registers(
        const DebugRegistersRequest& request, bool& invalidState, bool& busy, std::string& error, bool& unavailable)
    {
        busy         = false;
        invalidState = false;
        unavailable  = false;
        std::unique_lock lock {_operationMutex, std::try_to_lock};
        if (!lock.owns_lock())
        {
            busy  = true;
            error = "another debug operation is in progress";
            return std::nullopt;
        }

        std::string architecture;
        if (!checkQmpArchitecture(architecture, error, unavailable))
        {
            return std::nullopt;
        }

        QmpStatusInfo current;
        if (!qmpQueryStatus(current, error, unavailable))
        {
            return std::nullopt;
        }
        if (current.status == "running")
        {
            invalidState = true;
            error        = "the guest must be paused before its registers can be read";
            return std::nullopt;
        }

        if (_stubActive.load() && !_attachedRsp)
        {
            std::string ignored;
            bool        ignoredUnavailable = false;
            if (!stopStub(ignored, ignoredUnavailable))
            {
                error       = ignored.empty() ? "the previous GDB stub could not be stopped" : ignored;
                unavailable = ignoredUnavailable;
                return std::nullopt;
            }
        }

        const bool attached = _attachedRsp != nullptr;
        if (!attached)
        {
            bool active = false;
            if (!hasActiveStub(active, error, unavailable))
            {
                return std::nullopt;
            }
            if (active)
            {
                invalidState = true;
                error        = "another GDB stub owns the debug channel";
                return std::nullopt;
            }
        }

        std::unique_ptr<RspClient> ownedRsp;
        RspClient*                 rsp = nullptr;
        if (attached)
        {
            rsp = _attachedRsp.get();
        }
        else
        {
            ownedRsp = makeRsp();
            RspEndpoint endpoint;
            if (!startStub(*ownedRsp, endpoint, error, unavailable))
            {
                return std::nullopt;
            }
            rsp = ownedRsp.get();
        }

        struct Cleanup
        {
            DebugController& self;
            bool             attached;

            ~Cleanup()
            {
                if (attached)
                {
                    return;
                }
                std::string ignored;
                bool        ignoredUnavailable = false;
                if (self._stubActive.load() && !self.stopStub(ignored, ignoredUnavailable))
                {
                    log::warn("host", "could not stop the GDB stub during teardown: ", ignored);
                }
                std::error_code removeError;
                std::filesystem::remove(self._stubSocket, removeError);

                QmpStatusInfo after;
                if (self.qmpQueryStatus(after, ignored, ignoredUnavailable) && after.status == "running")
                {
                    std::string restore;
                    if (!self.qmpExecute("stop", restore, ignored, ignoredUnavailable))
                    {
                        log::warn("host", "could not restore the paused guest after reading registers: ", ignored);
                    }
                }
            }
        } cleanup {*this, attached};

        if (attached)
        {
            return readRecords(*rsp, request, architecture, error, unavailable, &_attachedDescription);
        }
        return readRecords(*rsp, request, architecture, error, unavailable);
    }

    std::optional<DebugRegisterRecord> DebugController::setRegister(std::uint64_t                  sessionId,
                                                                    const DebugSetRegisterRequest& request,
                                                                    bool&                          invalidState,
                                                                    bool&                          busy,
                                                                    std::string&                   error,
                                                                    bool&                          unavailable)
    {
        invalidState = false;
        busy         = false;
        unavailable  = false;

        std::unique_lock lock {_operationMutex, std::try_to_lock};
        if (!lock.owns_lock())
        {
            busy  = true;
            error = "another debug operation is in progress";
            return std::nullopt;
        }
        if (!ownsAttachment(sessionId, invalidState, error))
        {
            return std::nullopt;
        }

        QmpStatusInfo current;
        if (!qmpQueryStatus(current, error, unavailable))
        {
            return std::nullopt;
        }
        if (_sessionRunning.load() || current.status == "running")
        {
            invalidState = true;
            error        = "the guest must be paused before a register can be written";
            return std::nullopt;
        }

        std::vector<std::string> threads;
        if (!_attachedRsp->queryThreads(threads, kRspTimeout, error))
        {
            return std::nullopt;
        }
        if (threads.empty())
        {
            invalidState = true;
            error        = "the GDB stub reported no threads";
            return std::nullopt;
        }

        std::uint32_t vcpu = request.vcpu;
        std::string   thread;
        if (!request.threadId.empty())
        {
            const auto found = std::find(threads.begin(), threads.end(), request.threadId);
            if (found == threads.end())
            {
                invalidState = true;
                error        = "the selected GDB thread is not present";
                return std::nullopt;
            }
            vcpu   = static_cast<std::uint32_t>(std::distance(threads.begin(), found));
            thread = request.threadId;
        }
        else
        {
            if (vcpu >= threads.size())
            {
                invalidState = true;
                error        = "vCPU " + std::to_string(vcpu) + " is not present";
                return std::nullopt;
            }
            thread = threads[vcpu];
        }

        if (!_attachedRsp->selectThread(thread, kRspTimeout, error))
        {
            return std::nullopt;
        }

        std::string registerName {request.name};
        std::ranges::transform(registerName,
                               registerName.begin(),
                               [](unsigned char character)
                               {
                                   return static_cast<char>(std::tolower(character));
                               });

        std::optional<RspRegisterInfo> info = findRspRegister(_attachedDescription, registerName);
        if (!info)
        {
            info = fallbackRspRegister(registerName);
        }
        if (!info)
        {
            invalidState = true;
            error        = "the GDB stub does not expose " + request.name;
            return std::nullopt;
        }

        const std::uint32_t bytes = std::clamp<std::uint32_t>((info->bitsize + 7) / 8, 1, sizeof(std::uint64_t));
        if (!_attachedRsp->writeRegister(info->number, request.value, bytes, kRspTimeout, error))
        {
            return std::nullopt;
        }

        auto record = readRecord(*_attachedRsp, _attachedDescription, vcpu, thread, error);
        if (!record)
        {
            return std::nullopt;
        }
        return record;
    }

    std::optional<std::vector<DebugThreadRecord>> DebugController::threads(
        const DebugThreadsRequest&, bool& invalidState, bool& busy, std::string& error, bool& unavailable)
    {
        busy         = false;
        invalidState = false;
        unavailable  = false;
        std::unique_lock lock {_operationMutex, std::try_to_lock};
        if (!lock.owns_lock())
        {
            busy  = true;
            error = "another debug operation is in progress";
            return std::nullopt;
        }

        std::string architecture;
        if (!checkQmpArchitecture(architecture, error, unavailable))
        {
            return std::nullopt;
        }

        QmpStatusInfo current;
        if (!qmpQueryStatus(current, error, unavailable))
        {
            return std::nullopt;
        }
        if (current.status == "running")
        {
            invalidState = true;
            error        = "the guest must be paused before its threads can be read";
            return std::nullopt;
        }

        if (_stubActive.load() && !_attachedRsp)
        {
            std::string ignored;
            bool        ignoredUnavailable = false;
            if (!stopStub(ignored, ignoredUnavailable))
            {
                error       = ignored.empty() ? "the previous GDB stub could not be stopped" : ignored;
                unavailable = ignoredUnavailable;
                return std::nullopt;
            }
        }

        const bool attached = _attachedRsp != nullptr;
        if (!attached)
        {
            bool active = false;
            if (!hasActiveStub(active, error, unavailable))
            {
                return std::nullopt;
            }
            if (active)
            {
                invalidState = true;
                error        = "another GDB stub owns the debug channel";
                return std::nullopt;
            }
        }

        std::unique_ptr<RspClient> ownedRsp;
        RspClient*                 rsp = nullptr;
        if (attached)
        {
            rsp = _attachedRsp.get();
        }
        else
        {
            ownedRsp = makeRsp();
            RspEndpoint endpoint;
            if (!startStub(*ownedRsp, endpoint, error, unavailable))
            {
                return std::nullopt;
            }
            rsp = ownedRsp.get();
        }

        struct Cleanup
        {
            DebugController& self;
            bool             attached;

            ~Cleanup()
            {
                if (attached)
                {
                    return;
                }
                std::string ignored;
                bool        ignoredUnavailable = false;
                if (self._stubActive.load() && !self.stopStub(ignored, ignoredUnavailable))
                {
                    log::warn("host", "could not stop the GDB stub during thread discovery teardown: ", ignored);
                }
                std::error_code removeError;
                std::filesystem::remove(self._stubSocket, removeError);

                QmpStatusInfo after;
                if (self.qmpQueryStatus(after, ignored, ignoredUnavailable) && after.status == "running")
                {
                    std::string restore;
                    if (!self.qmpExecute("stop", restore, ignored, ignoredUnavailable))
                    {
                        log::warn("host", "could not restore the paused guest after thread discovery: ", ignored);
                    }
                }
            }
        } cleanup {*this, attached};

        DebugRegistersRequest all;
        const auto            records =
            readRecords(*rsp, all, architecture, error, unavailable, attached ? &_attachedDescription : nullptr);
        if (!records)
        {
            return std::nullopt;
        }

        std::vector<DebugThreadRecord> threads;
        threads.reserve(records->size());
        for (const auto& record : *records)
        {
            DebugThreadRecord thread;
            thread.vcpu         = record.vcpu;
            thread.threadId     = record.threadId;
            thread.rip          = record.rip;
            thread.rsp          = record.rsp;
            thread.cr3          = record.cr3;
            thread.processFound = record.processFound;
            thread.pid          = record.guestPid;
            thread.threadFound  = record.threadFound;
            thread.tid          = record.guestTid;
            threads.push_back(std::move(thread));
        }
        return threads;
    }

    std::optional<DebugBacktraceResponse> DebugController::backtrace(
        const DebugBacktraceRequest& request, bool& invalidState, bool& busy, std::string& error, bool& unavailable)
    {
        invalidState = false;
        busy         = false;
        unavailable  = false;
        if (_inspector == nullptr)
        {
            error = "the backtrace service is unavailable";
            return std::nullopt;
        }
        if (request.maxFrames == 0 || request.maxFrames > kMaxDebugBacktraceFrames)
        {
            error = "the backtrace frame limit is out of range";
            return std::nullopt;
        }

        // Reuse the register reader's stub lifecycle and paused-guest checks. A thread id selects one thread
        // out of the full list; otherwise the vCPU index selects the record directly.
        DebugRegistersRequest registersRequest;
        registersRequest.vcpu = request.threadId.empty() ? request.vcpu : kDebugAllVcpus;
        const auto records    = registers(registersRequest, invalidState, busy, error, unavailable);
        if (!records)
        {
            return std::nullopt;
        }
        if (records->empty())
        {
            error = "the GDB stub returned no registers";
            return std::nullopt;
        }

        const DebugRegisterRecord* selected = nullptr;
        if (request.threadId.empty())
        {
            selected = &records->front();
        }
        else
        {
            for (const auto& record : *records)
            {
                if (record.threadId == request.threadId)
                {
                    selected = &record;
                    break;
                }
            }
        }
        if (selected == nullptr)
        {
            invalidState = true;
            error        = "thread " + request.threadId + " is not present in the stub's thread list";
            return std::nullopt;
        }

        const auto makeFrame = [this, &request](std::uint64_t address, std::uint64_t framePointer)
        {
            DebugBacktraceFrame frame;
            frame.address      = address;
            frame.framePointer = framePointer;

            DebugLocation location;
            resolveLocation(_inspector, request.pid, address, location);
            frame.module          = std::move(location.module);
            frame.moduleOffset    = location.moduleOffset;
            frame.exportFound     = location.exportFound;
            frame.exportAddress   = location.exportAddress;
            frame.exportOffset    = location.exportOffset;
            frame.exportName      = std::move(location.exportName);
            frame.pdbFound        = location.pdbFound;
            frame.pdbSymbol       = std::move(location.pdbSymbol);
            frame.pdbDisplacement = location.pdbDisplacement;
            return frame;
        };

        const auto readStackValue = [this, &request](std::uint64_t address, std::uint64_t& value, std::string& why)
        {
            std::vector<std::byte> raw;
            std::string            readError;
            if (!_inspector->readMemory(request.pid, address, 8, raw, readError))
            {
                char buffer[24] {};
                std::snprintf(buffer, sizeof(buffer), "%llx", static_cast<unsigned long long>(address));
                why = "the frame at 0x" + std::string {buffer} + " could not be read";
                return false;
            }
            if (raw.size() < 8)
            {
                why = "the frame ended before a saved pointer and return address";
                return false;
            }
            value = 0;
            for (std::size_t index = 0; index < 8; ++index)
            {
                value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(raw[index])) << (index * 8);
            }
            return true;
        };

        DebugBacktraceResponse response;
        response.frames.push_back(makeFrame(selected->rip, selected->rbp));

        std::uint64_t rbp = selected->rbp;
        while (response.frames.size() < request.maxFrames)
        {
            if (rbp == 0)
            {
                break;
            }
            const auto stopTruncated = [&response](std::string reason)
            {
                response.truncated  = true;
                response.stopReason = std::move(reason);
            };

            if ((rbp & 0x7) != 0)
            {
                stopTruncated("the frame pointer is not 8-byte aligned");
                break;
            }
            if (rbp < selected->rsp || rbp - selected->rsp > kMaxDebugBacktraceDepthBytes)
            {
                stopTruncated("the frame pointer is outside the stack depth limit");
                break;
            }
            if (rbp > std::numeric_limits<std::uint64_t>::max() - 8)
            {
                stopTruncated("the frame pointer overflows the address space");
                break;
            }

            std::uint64_t nextRbp       = 0;
            std::uint64_t returnAddress = 0;
            std::string   why;
            if (!readStackValue(rbp, nextRbp, why) || !readStackValue(rbp + 8, returnAddress, why))
            {
                stopTruncated(why);
                break;
            }

            // A null saved frame pointer is the normal end of a frame chain. A nonzero pointer that does not
            // advance is corrupt, and is not followed.
            if (nextRbp != 0)
            {
                if (nextRbp <= rbp)
                {
                    stopTruncated("the saved frame pointer did not advance");
                    break;
                }
                if ((nextRbp & 0x7) != 0)
                {
                    stopTruncated("the saved frame pointer is not 8-byte aligned");
                    break;
                }
                if (nextRbp < selected->rsp || nextRbp - selected->rsp > kMaxDebugBacktraceDepthBytes)
                {
                    stopTruncated("the saved frame pointer is outside the stack depth limit");
                    break;
                }
            }
            if (returnAddress == 0)
            {
                stopTruncated("the return address is zero");
                break;
            }

            response.frames.push_back(makeFrame(returnAddress, nextRbp));
            rbp = nextRbp;
            if (nextRbp == 0)
            {
                break;
            }
        }

        if (!response.truncated && response.frames.size() >= request.maxFrames
            && response.frames.back().framePointer != 0)
        {
            response.truncated  = true;
            response.stopReason = "the frame limit was reached";
        }
        return response;
    }

    std::optional<DebugWatchEnd> DebugController::watch(DebugWatchRequest              request,
                                                        const DebugWatchHitSink&       onHit,
                                                        const DebugWatchHeartbeatSink& onHeartbeat,
                                                        const DebugWatchAbortCheck&    aborted,
                                                        bool&                          busy,
                                                        std::string&                   error,
                                                        bool&                          unavailable,
                                                        bool&                          notFound)
    {
        busy        = false;
        unavailable = false;
        notFound    = false;
        std::unique_lock lock {_operationMutex, std::try_to_lock};
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
        if (!resolveRequestAddress(
                request.pid, request.module, request.symbol, request.address, request.address, notFound, error))
        {
            return std::nullopt;
        }
        _watchAccess   = request.access;
        // This watch owns the guest's run state from here; a pause requested before it started is not an
        // external pause for this run.
        _controlPaused = false;

        std::string architecture;
        if (!checkQmpArchitecture(architecture, error, unavailable))
        {
            return std::nullopt;
        }

        QmpStatusInfo current;
        if (!qmpQueryStatus(current, error, unavailable))
        {
            return std::nullopt;
        }

        bool stoppedByUs = false;
        if (current.status == "running")
        {
            std::string ignored;
            if (!qmpExecute("stop", ignored, error, unavailable))
            {
                return std::nullopt;
            }
            _hostPaused = true;
            stoppedByUs = true;
            log::info("host", "paused the guest for a watchpoint");
        }

        if (_stubActive.load() && !_attachedRsp)
        {
            std::string ignored;
            bool        ignoredUnavailable = false;
            if (!stopStub(ignored, ignoredUnavailable))
            {
                error       = ignored.empty() ? "the previous GDB stub could not be stopped" : ignored;
                unavailable = ignoredUnavailable;
                return std::nullopt;
            }
        }

        const bool attached = _attachedRsp != nullptr;
        if (!attached)
        {
            bool active = false;
            if (!hasActiveStub(active, error, unavailable))
            {
                return std::nullopt;
            }
            if (active)
            {
                error = "another GDB stub owns the debug channel";
                return std::nullopt;
            }
        }

        std::unique_ptr<RspClient> ownedRsp;
        RspClient*                 rsp = nullptr;
        if (attached)
        {
            rsp = _attachedRsp.get();
        }
        else
        {
            ownedRsp = makeRsp();
            RspEndpoint endpoint;
            if (!startStub(*ownedRsp, endpoint, error, unavailable))
            {
                return std::nullopt;
            }
            rsp = ownedRsp.get();
        }

        // QEMU's gdbstub keeps one global debug-register set: a second Z2 for the same address on another vCPU
        // is rejected, so the watchpoint is armed once rather than once per thread.
        std::optional<std::string> armedThread;

        struct Cleanup
        {
            DebugController&            self;
            RspClient&                  rsp;
            std::optional<std::string>& armedThread;
            const DebugWatchRequest&    request;
            bool                        stoppedByUs;
            bool                        attached;

            ~Cleanup()
            {
                std::string   ignored;
                bool          ignoredUnavailable = false;
                // The gdbstub refuses most packets while the target is running, so stop it before touching the
                // debug registers. This is separate from whether the request itself paused the guest.
                QmpStatusInfo beforeRemove;
                if (self.qmpQueryStatus(beforeRemove, ignored, ignoredUnavailable) && beforeRemove.status == "running")
                {
                    std::string ignoredStop;
                    if (!self.qmpExecute("stop", ignoredStop, ignored, ignoredUnavailable))
                    {
                        log::warn("host", "could not pause the guest to remove the watchpoint: ", ignored);
                    }
                }
                rsp.drainPending();
                if (armedThread)
                {
                    // The watchpoint is global, so removal does not depend on which thread is selected. Retry
                    // once after dropping a stale reply: leaving a debug register armed breaks the next watch.
                    if (!rsp.removeWatchpoint(request.address, request.width, request.access, kRspTimeout, ignored))
                    {
                        rsp.drainPending();
                        if (!rsp.removeWatchpoint(request.address, request.width, request.access, kRspTimeout, ignored))
                        {
                            log::warn("host", "could not remove the watchpoint: ", ignored);
                        }
                    }
                }
                self._watchCount = 0;

                if (!attached)
                {
                    // Detach the RSP client before stopping the stub; otherwise QEMU can stay in its gdb stop
                    // state and the QMP continue below has no effect.
                    rsp.disconnect();

                    if (self._stubActive.load() && !self.stopStub(ignored, ignoredUnavailable))
                    {
                        log::warn("host", "could not stop the GDB stub during watchpoint teardown: ", ignored);
                    }
                    std::error_code removeError;
                    std::filesystem::remove(self._stubSocket, removeError);
                }

                QmpStatusInfo after;
                if (stoppedByUs)
                {
                    if (self._controlPaused.load())
                    {
                        // Another client asked for a pause during the watch; leave the guest as it asked.
                        log::info("host", "leaving the guest paused after a watchpoint");
                    }
                    else
                    {
                        if (self.qmpQueryStatus(after, ignored, ignoredUnavailable) && after.status != "running")
                        {
                            if (!self.qmpExecute("cont", ignored, ignored, ignoredUnavailable))
                            {
                                log::warn("host", "could not resume the guest after a watchpoint: ", ignored);
                            }
                        }
                        QmpStatusInfo verified;
                        if (self.qmpQueryStatus(verified, ignored, ignoredUnavailable) && verified.status != "running")
                        {
                            log::warn("host", "the guest is still not running after a watchpoint");
                        }
                        self._hostPaused = false;
                    }
                }
                else if (self.qmpQueryStatus(after, ignored, ignoredUnavailable) && after.status == "running")
                {
                    if (!self.qmpExecute("stop", ignored, ignored, ignoredUnavailable))
                    {
                        log::warn("host", "could not restore the paused guest after a watchpoint: ", ignored);
                    }
                }
            }
        } cleanup {*this, *rsp, armedThread, request, stoppedByUs, attached};

        RspTargetDescription description;
        if (attached)
        {
            description = _attachedDescription;
        }
        else
        {
            if (!rsp->negotiate(kRspTimeout, error))
            {
                return std::nullopt;
            }
            if (!rsp->readTargetDescription(description, kRspTimeout, error))
            {
                return std::nullopt;
            }
            if (architecture.empty())
            {
                architecture = architectureFromTargetDescription(description);
                if (!isSupportedArchitecture(architecture))
                {
                    rejectArchitecture(architecture, true, error, unavailable);
                    return std::nullopt;
                }
            }
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
        bool unsupported = false;
        if (!rsp->insertWatchpoint(request.address, request.width, request.access, kRspTimeout, error, unsupported))
        {
            if (unsupported)
            {
                unavailable = true;
            }
            return std::nullopt;
        }
        armedThread = threads.front();
        _watchCount = 1;
        log::info("host", "armed a ", debugWatchAccessName(request.access), " watchpoint");

        // Drop any stop reply QEMU queued before this client attached, so the first poll cannot report
        // stale registers as a hit.
        rsp->drainPending();

        DebugWatchEnd end;
        end.status                  = DebugWatchStatus::Completed;
        end.access                  = request.access;
        std::uint32_t hits          = 0;
        const auto    watchStart    = std::chrono::steady_clock::now();
        const auto    watchDeadline = watchStart + std::chrono::milliseconds {request.timeoutMs};
        auto          nextHeartbeat = watchStart + std::chrono::milliseconds {kDebugWatchHeartbeatMs};

        const auto emitHeartbeat = [&]() -> bool
        {
            if (!onHeartbeat)
            {
                return true;
            }
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - watchStart)
                    .count();
            DebugWatchHeartbeat heartbeat;
            heartbeat.elapsedMs    = static_cast<std::uint32_t>(std::min<std::int64_t>(elapsed, 0xFFFFFFFF));
            heartbeat.hitCount     = hits;
            heartbeat.skippedCount = end.skippedCount;
            heartbeat.access       = request.access;
            if (!onHeartbeat(heartbeat))
            {
                end.status = DebugWatchStatus::Cancelled;
                end.error  = "the watchpoint heartbeat could not be delivered";
                return false;
            }
            return true;
        };

        const auto maybeEmitHeartbeat = [&]() -> bool
        {
            const auto now = std::chrono::steady_clock::now();
            if (now < nextHeartbeat)
            {
                return true;
            }
            if (!emitHeartbeat())
            {
                return false;
            }
            nextHeartbeat = now + std::chrono::milliseconds {kDebugWatchHeartbeatMs};
            return true;
        };

        // A debug-stop from another client pauses the guest through QMP, which can surface as a stub stop.
        // Stay paused until that client continues instead of resuming the guest behind its back.
        const auto waitForControlResume = [&]() -> bool
        {
            while (_controlPaused.load())
            {
                if (aborted && aborted())
                {
                    end.status = DebugWatchStatus::Cancelled;
                    end.error  = "the watchpoint was cancelled";
                    return false;
                }
                if (requestExpired())
                {
                    end.status = DebugWatchStatus::Timeout;
                    end.error  = "the request deadline expired";
                    return false;
                }
                if (std::chrono::steady_clock::now() >= watchDeadline)
                {
                    end.status = DebugWatchStatus::Timeout;
                    end.error  = "the watchpoint timed out";
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds {10});
                if (!maybeEmitHeartbeat())
                {
                    return false;
                }
            }
            return true;
        };

        if (!waitForControlResume())
        {
            return end;
        }
        if (!rsp->continueExecution(kRspTimeout, error))
        {
            end.status = DebugWatchStatus::Error;
            end.error  = error;
            return end;
        }

        while (request.maxHits == 0 || hits < request.maxHits)
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
                if (!maybeEmitHeartbeat())
                {
                    break;
                }
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

            std::uint64_t newValue = 0;
            if (!readWatchValue(_inspector, request.pid, request.address, request.width, newValue, error))
            {
                end.status = DebugWatchStatus::Error;
                end.error  = error;
                break;
            }

            // A QEMU stop for a write watchpoint names the watched linear address. Accept it when that
            // address and the value both confirm the hit; the process DTB and module maps are not reliable
            // while a user-mode stop is in flight. Older or different stubs may omit the field, so keep the
            // CR3/module fallback for them. A user-mode CR3 may be a shadow page table that MemProcFS cannot
            // report for the process, so a CR3 mismatch alone is not proof that the hit belongs to someone
            // else.
            const auto stopAddress  = stopWatchAddress(stopPacket);
            bool       accept       = false;
            const bool valueChanged = newValue != lastValue;
            if (stopAddress.has_value())
            {
                // Read and access watchpoints fire without changing memory, so only a write watch needs the
                // extra value check.
                accept = *stopAddress == request.address && (request.access != DebugWatchAccess::Write || valueChanged);
            }
            else
            {
                const auto hitDtb = canonicalDtb(cr3);
                accept            = hitDtb == canonicalDtb(dtbs->dtb) || hitDtb == canonicalDtb(dtbs->dtbUser);
                if (!accept)
                {
                    std::string moduleError;
                    accept = (request.access != DebugWatchAccess::Write || valueChanged)
                          && _inspector->moduleForAddress(request.pid, rip, moduleError).has_value();
                }
            }

            if (!accept)
            {
                ++end.skippedCount;
                if (!waitForControlResume())
                {
                    break;
                }
                if (!rsp->continueExecution(kRspTimeout, error))
                {
                    end.status = DebugWatchStatus::Error;
                    end.error  = error;
                    break;
                }
                continue;
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
            resolveGuestContext(cr3, rip, rspValue, hit.guestPid, hit.guestTid, hit.processFound, hit.threadFound);

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
            if (request.maxHits != 0 && hits >= request.maxHits)
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

    std::optional<DebugBreakpointResponse> DebugController::breakpoint(DebugBreakpointRequest      request,
                                                                       const DebugWatchAbortCheck& aborted,
                                                                       bool&                       invalidState,
                                                                       bool&                       busy,
                                                                       std::string&                error,
                                                                       bool&                       unavailable,
                                                                       bool&                       notFound)
    {
        busy         = false;
        invalidState = false;
        unavailable  = false;
        notFound     = false;
        std::unique_lock lock {_operationMutex, std::try_to_lock};
        if (!lock.owns_lock())
        {
            busy  = true;
            error = "another debug operation is in progress";
            return std::nullopt;
        }
        if (_inspector == nullptr)
        {
            error = "the breakpoint service is unavailable";
            return std::nullopt;
        }
        if (!resolveRequestAddress(
                request.pid, request.module, request.symbol, request.address, request.address, notFound, error))
        {
            return std::nullopt;
        }
        // This breakpoint owns the guest's run state from here; see the watch path for the same reset.
        _controlPaused = false;

        std::string architecture;
        if (!checkQmpArchitecture(architecture, error, unavailable))
        {
            return std::nullopt;
        }

        QmpStatusInfo current;
        if (!qmpQueryStatus(current, error, unavailable))
        {
            return std::nullopt;
        }

        bool stoppedByUs = false;
        if (current.status == "running")
        {
            std::string ignored;
            if (!qmpExecute("stop", ignored, error, unavailable))
            {
                return std::nullopt;
            }
            _hostPaused = true;
            stoppedByUs = true;
            log::info("host", "paused the guest for a breakpoint");
        }

        if (_stubActive.load() && !_attachedRsp)
        {
            std::string ignored;
            bool        ignoredUnavailable = false;
            if (!stopStub(ignored, ignoredUnavailable))
            {
                error       = ignored.empty() ? "the previous GDB stub could not be stopped" : ignored;
                unavailable = ignoredUnavailable;
                return std::nullopt;
            }
        }

        const bool attached = _attachedRsp != nullptr;
        if (!attached)
        {
            bool active = false;
            if (!hasActiveStub(active, error, unavailable))
            {
                return std::nullopt;
            }
            if (active)
            {
                invalidState = true;
                error        = "another GDB stub owns the debug channel";
                return std::nullopt;
            }
        }

        std::unique_ptr<RspClient> ownedRsp;
        RspClient*                 rsp = nullptr;
        if (attached)
        {
            rsp = _attachedRsp.get();
        }
        else
        {
            ownedRsp = makeRsp();
            RspEndpoint endpoint;
            if (!startStub(*ownedRsp, endpoint, error, unavailable))
            {
                return std::nullopt;
            }
            rsp = ownedRsp.get();
        }

        const bool hardware = request.kind == DebugBreakpointKind::Hardware;
        bool       armed    = false;

        struct Cleanup
        {
            DebugController&              self;
            RspClient&                    rsp;
            const DebugBreakpointRequest& request;
            bool                          armed;
            bool                          stoppedByUs;
            bool                          attached;

            ~Cleanup()
            {
                std::string   ignored;
                bool          ignoredUnavailable = false;
                // Removing a breakpoint needs the target stopped, just like removing a watchpoint.
                QmpStatusInfo beforeRemove;
                if (self.qmpQueryStatus(beforeRemove, ignored, ignoredUnavailable) && beforeRemove.status == "running")
                {
                    std::string ignoredStop;
                    if (!self.qmpExecute("stop", ignoredStop, ignored, ignoredUnavailable))
                    {
                        log::warn("host", "could not pause the guest to remove the breakpoint: ", ignored);
                    }
                }
                rsp.drainPending();
                if (armed)
                {
                    const bool hardwareKind = request.kind == DebugBreakpointKind::Hardware;
                    // Retry once after dropping a stale reply: a debug register left set breaks the next
                    // debug operation.
                    if (!rsp.removeBreakpoint(request.address, hardwareKind, kRspTimeout, ignored))
                    {
                        rsp.drainPending();
                        if (!rsp.removeBreakpoint(request.address, hardwareKind, kRspTimeout, ignored))
                        {
                            log::warn("host", "could not remove the breakpoint: ", ignored);
                        }
                    }
                }

                if (!attached)
                {
                    rsp.disconnect();
                    if (self._stubActive.load() && !self.stopStub(ignored, ignoredUnavailable))
                    {
                        log::warn("host", "could not stop the GDB stub during breakpoint teardown: ", ignored);
                    }
                    std::error_code removeError;
                    std::filesystem::remove(self._stubSocket, removeError);
                }

                QmpStatusInfo after;
                if (stoppedByUs)
                {
                    if (self._controlPaused.load())
                    {
                        // Another client asked for a pause during the breakpoint; leave the guest paused.
                        log::info("host", "leaving the guest paused after a breakpoint");
                    }
                    else
                    {
                        if (self.qmpQueryStatus(after, ignored, ignoredUnavailable) && after.status != "running")
                        {
                            if (!self.qmpExecute("cont", ignored, ignored, ignoredUnavailable))
                            {
                                log::warn("host", "could not resume the guest after a breakpoint: ", ignored);
                            }
                        }
                        QmpStatusInfo verified;
                        if (self.qmpQueryStatus(verified, ignored, ignoredUnavailable) && verified.status != "running")
                        {
                            log::warn("host", "the guest is still not running after a breakpoint");
                        }
                        self._hostPaused = false;
                    }
                }
                else if (self.qmpQueryStatus(after, ignored, ignoredUnavailable) && after.status == "running")
                {
                    if (!self.qmpExecute("stop", ignored, ignored, ignoredUnavailable))
                    {
                        log::warn("host", "could not restore the paused guest after a breakpoint: ", ignored);
                    }
                }
            }
        } cleanup {*this, *rsp, request, armed, stoppedByUs, attached};

        RspTargetDescription description;
        if (attached)
        {
            description = _attachedDescription;
        }
        else
        {
            if (!rsp->negotiate(kRspTimeout, error))
            {
                return std::nullopt;
            }
            if (!rsp->readTargetDescription(description, kRspTimeout, error))
            {
                return std::nullopt;
            }
            if (architecture.empty())
            {
                architecture = architectureFromTargetDescription(description);
                if (!isSupportedArchitecture(architecture))
                {
                    rejectArchitecture(architecture, true, error, unavailable);
                    return std::nullopt;
                }
            }
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

        std::string selectedThread = threads.front();
        if (!hardware && isUserModeAddress(request.address))
        {
            const auto selection =
                selectThreadInProcess(*rsp, description, threads, dtbs->dtb, dtbs->dtbUser, selectedThread, error);
            if (selection == ThreadSelection::Error)
            {
                return std::nullopt;
            }
            if (selection == ThreadSelection::NotInProcess)
            {
                invalidState = true;
                return std::nullopt;
            }
        }
        if (!rsp->selectThread(selectedThread, kRspTimeout, error))
        {
            return std::nullopt;
        }
        if (!rsp->insertBreakpoint(request.address, hardware, kRspTimeout, error))
        {
            return std::nullopt;
        }
        armed         = true;
        cleanup.armed = true;
        log::info("host", "armed a ", hardware ? "hardware" : "software", " breakpoint");

        // A stop reply queued before this client attached must not be mistaken for a breakpoint hit.
        rsp->drainPending();

        DebugBreakpointResponse response;
        response.address = request.address;
        const auto breakpointDeadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds {request.timeoutMs};

        const auto waitForControlResume = [&]() -> bool
        {
            while (_controlPaused.load())
            {
                if (aborted && aborted())
                {
                    response.status = DebugBreakpointStatus::Cancelled;
                    response.error  = "the breakpoint was cancelled";
                    return false;
                }
                if (requestExpired())
                {
                    response.status = DebugBreakpointStatus::Timeout;
                    response.error  = "the request deadline expired";
                    return false;
                }
                if (std::chrono::steady_clock::now() >= breakpointDeadline)
                {
                    response.status = DebugBreakpointStatus::Timeout;
                    response.error  = "the breakpoint timed out";
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds {10});
            }
            return true;
        };

        if (!waitForControlResume())
        {
            return response;
        }
        if (!rsp->continueExecution(kRspTimeout, error))
        {
            response.status = DebugBreakpointStatus::Error;
            response.error  = error;
            return response;
        }

        while (true)
        {
            if (aborted && aborted())
            {
                response.status = DebugBreakpointStatus::Cancelled;
                response.error  = "the breakpoint was cancelled";
                break;
            }
            if (requestExpired())
            {
                response.status = DebugBreakpointStatus::Timeout;
                response.error  = "the request deadline expired";
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= breakpointDeadline)
            {
                response.status = DebugBreakpointStatus::Timeout;
                response.error  = "the breakpoint timed out";
                break;
            }
            const auto slice = std::min(std::chrono::duration_cast<std::chrono::milliseconds>(breakpointDeadline - now),
                                        std::chrono::milliseconds {100});

            std::string stopPacket;
            const auto  polled = rsp->pollPacket(stopPacket, slice, error);
            if (polled == RspPollResult::Timeout)
            {
                continue;
            }
            if (polled == RspPollResult::Closed)
            {
                response.status = DebugBreakpointStatus::Error;
                response.error  = error.empty() ? "the GDB stub closed the connection" : error;
                break;
            }
            if (!stopPacket.empty() && (stopPacket.front() == 'W' || stopPacket.front() == 'X'))
            {
                response.status = DebugBreakpointStatus::Error;
                response.error  = "the guest stopped before the breakpoint fired";
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
                response.status = DebugBreakpointStatus::Error;
                response.error  = error;
                break;
            }

            std::uint64_t rip      = 0;
            std::uint64_t rspValue = 0;
            std::uint64_t cr3      = 0;
            if (!readNamedRegister(*rsp, description, "rip", rip, kRspTimeout, error)
                || !readNamedRegister(*rsp, description, "rsp", rspValue, kRspTimeout, error)
                || !readNamedRegister(*rsp, description, "cr3", cr3, kRspTimeout, error))
            {
                response.status = DebugBreakpointStatus::Error;
                response.error  = error;
                break;
            }

            // The stub's breakpoint is global to the VM, and vCont can also deliver an unrelated stop that is not
            // caused by the breakpoint. Require the PC to be at the requested address. A hardware breakpoint on
            // a shared user address traps in every process that maps it, so a matching CR3 is the only verified
            // attribution; a stop whose CR3 is a shadow page table MemProcFS does not report may still lie inside
            // one of the target's modules, but it is reported as unverified rather than accepted silently.
            bool matches    = false;
            bool unverified = false;
            bool moduleHit  = false;
            if (rip == request.address)
            {
                const auto hitDtb = canonicalDtb(cr3);
                if (hitDtb == canonicalDtb(dtbs->dtb) || hitDtb == canonicalDtb(dtbs->dtbUser))
                {
                    matches = true;
                }
                else
                {
                    std::string moduleError;
                    moduleHit = _inspector->moduleForAddress(request.pid, request.address, moduleError).has_value();
                    if (moduleHit)
                    {
                        matches    = true;
                        unverified = hardware;
                    }
                }
            }
            if (!matches)
            {
                if (response.skippedCount == 0)
                {
                    log::info("host",
                              "skipping breakpoint stop: rip=",
                              rip,
                              " requested=",
                              request.address,
                              " cr3=",
                              cr3,
                              " dtb=",
                              dtbs->dtb,
                              " dtbUser=",
                              dtbs->dtbUser,
                              " module=",
                              moduleHit);
                }
                ++response.skippedCount;
                if (!waitForControlResume())
                {
                    break;
                }
                if (!rsp->continueExecution(kRspTimeout, error))
                {
                    response.status = DebugBreakpointStatus::Error;
                    response.error  = error;
                    break;
                }
                continue;
            }

            response.status     = DebugBreakpointStatus::Hit;
            response.vcpu       = 0;
            response.rip        = rip;
            response.rsp        = rspValue;
            response.cr3        = cr3;
            response.unverified = unverified;
            response.threadId   = thread ? *thread : std::string {};
            if (!response.threadId.empty())
            {
                const auto found = std::find(threads.begin(), threads.end(), response.threadId);
                if (found != threads.end())
                {
                    response.vcpu = static_cast<std::uint32_t>(std::distance(threads.begin(), found));
                }
            }
            resolveGuestContext(
                cr3, rip, rspValue, response.guestPid, response.guestTid, response.processFound, response.threadFound);

            std::string readError;
            if (!_inspector->readMemory(request.pid, rip, kDebugBreakpointBytes, response.bytesAtRip, readError))
            {
                response.bytesAtRip.clear();
            }

            DebugLocation location;
            resolveLocation(_inspector, request.pid, rip, location);
            response.module          = std::move(location.module);
            response.moduleOffset    = location.moduleOffset;
            response.exportFound     = location.exportFound;
            response.exportAddress   = location.exportAddress;
            response.exportOffset    = location.exportOffset;
            response.exportName      = std::move(location.exportName);
            response.pdbFound        = location.pdbFound;
            response.pdbSymbol       = std::move(location.pdbSymbol);
            response.pdbDisplacement = location.pdbDisplacement;
            break;
        }

        return response;
    }

    std::optional<DebugStepResponse> DebugController::step(
        const DebugStepRequest& request, bool& invalidState, bool& busy, std::string& error, bool& unavailable)
    {
        busy         = false;
        invalidState = false;
        unavailable  = false;
        std::unique_lock lock {_operationMutex, std::try_to_lock};
        if (!lock.owns_lock())
        {
            busy  = true;
            error = "another debug operation is in progress";
            return std::nullopt;
        }

        std::string architecture;
        if (!checkQmpArchitecture(architecture, error, unavailable))
        {
            return std::nullopt;
        }

        QmpStatusInfo current;
        if (!qmpQueryStatus(current, error, unavailable))
        {
            return std::nullopt;
        }
        if (current.status == "running")
        {
            invalidState = true;
            error        = "the guest must be paused before it can be single-stepped";
            return std::nullopt;
        }

        if (_stubActive.load() && !_attachedRsp)
        {
            std::string ignored;
            bool        ignoredUnavailable = false;
            if (!stopStub(ignored, ignoredUnavailable))
            {
                error       = ignored.empty() ? "the previous GDB stub could not be stopped" : ignored;
                unavailable = ignoredUnavailable;
                return std::nullopt;
            }
        }

        const bool attached = _attachedRsp != nullptr;
        if (!attached)
        {
            bool active = false;
            if (!hasActiveStub(active, error, unavailable))
            {
                return std::nullopt;
            }
            if (active)
            {
                invalidState = true;
                error        = "another GDB stub owns the debug channel";
                return std::nullopt;
            }
        }

        std::unique_ptr<RspClient> ownedRsp;
        RspClient*                 rsp = nullptr;
        if (attached)
        {
            rsp = _attachedRsp.get();
        }
        else
        {
            ownedRsp = makeRsp();
            RspEndpoint endpoint;
            if (!startStub(*ownedRsp, endpoint, error, unavailable))
            {
                return std::nullopt;
            }
            rsp = ownedRsp.get();
        }

        struct Cleanup
        {
            DebugController& self;
            bool             attached;

            ~Cleanup()
            {
                if (attached)
                {
                    return;
                }
                std::string ignored;
                bool        ignoredUnavailable = false;
                if (self._stubActive.load() && !self.stopStub(ignored, ignoredUnavailable))
                {
                    log::warn("host", "could not stop the GDB stub during single-step teardown: ", ignored);
                }
                std::error_code removeError;
                std::filesystem::remove(self._stubSocket, removeError);

                // A single step is only valid from a paused guest and leaves it paused.
                QmpStatusInfo after;
                if (self.qmpQueryStatus(after, ignored, ignoredUnavailable) && after.status == "running")
                {
                    std::string restore;
                    if (!self.qmpExecute("stop", restore, ignored, ignoredUnavailable))
                    {
                        log::warn("host", "could not restore the paused guest after a single step: ", ignored);
                    }
                }
            }
        } cleanup {*this, attached};

        RspTargetDescription description;
        if (attached)
        {
            description = _attachedDescription;
        }
        else
        {
            if (!rsp->negotiate(kRspTimeout, error))
            {
                return std::nullopt;
            }
            if (!rsp->readTargetDescription(description, kRspTimeout, error))
            {
                return std::nullopt;
            }
            if (architecture.empty())
            {
                architecture = architectureFromTargetDescription(description);
                if (!isSupportedArchitecture(architecture))
                {
                    rejectArchitecture(architecture, true, error, unavailable);
                    return std::nullopt;
                }
            }
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
        std::uint32_t selectedIndex = request.vcpu;
        if (!request.threadId.empty())
        {
            const auto found = std::find(threads.begin(), threads.end(), request.threadId);
            if (found == threads.end())
            {
                invalidState = true;
                error        = "thread '" + request.threadId + "' is not present";
                return std::nullopt;
            }
            selectedIndex = static_cast<std::uint32_t>(std::distance(threads.begin(), found));
        }
        else if (request.vcpu >= threads.size())
        {
            error = "vCPU " + std::to_string(request.vcpu) + " is not present";
            return std::nullopt;
        }

        const auto& thread = threads[selectedIndex];
        if (!rsp->selectThread(thread, kRspTimeout, error))
        {
            return std::nullopt;
        }
        rsp->drainPending();
        if (!rsp->stepExecution(thread, kRspTimeout, error))
        {
            return std::nullopt;
        }

        const auto stepDeadline = std::chrono::steady_clock::now() + kRspTimeout;
        while (true)
        {
            if (requestExpired())
            {
                error = "the request deadline expired";
                return std::nullopt;
            }
            if (std::chrono::steady_clock::now() >= stepDeadline)
            {
                error = "the guest did not stop after a single step";
                return std::nullopt;
            }

            std::string stopPacket;
            const auto  polled = rsp->pollPacket(stopPacket, std::chrono::milliseconds {100}, error);
            if (polled == RspPollResult::Timeout)
            {
                continue;
            }
            if (polled == RspPollResult::Closed)
            {
                error = error.empty() ? "the GDB stub closed the connection" : error;
                return std::nullopt;
            }
            if (!stopPacket.empty() && (stopPacket.front() == 'W' || stopPacket.front() == 'X'))
            {
                error = "the guest stopped before the single step completed";
                return std::nullopt;
            }

            bool       stopped    = false;
            const auto stopThread = stopThreadId(stopPacket, stopped);
            if (!stopped)
            {
                continue;
            }
            if (stopThread && !stopThread->empty() && !rsp->selectThread(*stopThread, kRspTimeout, error))
            {
                return std::nullopt;
            }

            auto index = selectedIndex;
            if (stopThread && !stopThread->empty())
            {
                const auto found = std::find(threads.begin(), threads.end(), *stopThread);
                if (found != threads.end())
                {
                    index = static_cast<std::uint32_t>(std::distance(threads.begin(), found));
                }
            }

            auto record = readRecord(*rsp, description, index, threads[index], error);
            if (!record)
            {
                return std::nullopt;
            }
            return DebugStepResponse {std::move(*record)};
        }
    }

    bool DebugController::ownsAttachment(std::uint64_t sessionId, bool& invalidState, std::string& error)
    {
        // The attachment is shared the same way register reads and one-shot resources are: any client bound to
        // the target may manage resources, while only the owner session ends the attachment when it closes.
        (void)sessionId;
        if (_attachedRsp == nullptr)
        {
            invalidState = true;
            error        = "attach the debugger before managing persistent breakpoints or watchpoints";
            return false;
        }
        return true;
    }

    bool DebugController::requirePausedForResource(bool& invalidState, std::string& error, bool& unavailable)
    {
        QmpStatusInfo current;
        if (!qmpQueryStatus(current, error, unavailable))
        {
            return false;
        }
        if (_sessionRunning.load() || current.status == "running")
        {
            invalidState = true;
            error        = "pause the guest before changing debug resources";
            return false;
        }
        return true;
    }

    std::uint32_t DebugController::allocateHardwareSlot() noexcept
    {
        for (std::uint32_t slot = 0; slot < kMaxDebugHardwareSlots; ++slot)
        {
            if ((_usedHardwareSlots & (1u << slot)) == 0)
            {
                _usedHardwareSlots |= 1u << slot;
                return slot;
            }
        }
        return kNoDebugHardwareSlot;
    }

    void DebugController::releaseHardwareSlot(std::uint32_t slot) noexcept
    {
        if (slot < kMaxDebugHardwareSlots)
        {
            _usedHardwareSlots &= ~(1u << slot);
        }
    }

    void DebugController::refreshWatchCount() noexcept
    {
        std::uint32_t count = 0;
        for (const auto& resource : _resources)
        {
            if (resource.kind == DebugResourceKind::Watchpoint)
            {
                ++count;
            }
        }
        _watchCount = count;
    }

    std::optional<DebugBreakpointArmResponse> DebugController::armBreakpoint(std::uint64_t                    sessionId,
                                                                             const DebugBreakpointArmRequest& request,
                                                                             bool&        invalidState,
                                                                             bool&        busy,
                                                                             std::string& error,
                                                                             bool&        unavailable)
    {
        busy         = false;
        invalidState = false;
        unavailable  = false;
        std::unique_lock lock {_operationMutex, std::try_to_lock};
        if (!lock.owns_lock())
        {
            busy  = true;
            error = "another debug operation is in progress";
            return std::nullopt;
        }
        if (!ownsAttachment(sessionId, invalidState, error))
        {
            return std::nullopt;
        }
        if (_resources.size() >= kMaxDebugArmedResources)
        {
            invalidState = true;
            error        = "no more debug resources can be armed";
            return std::nullopt;
        }
        if (!requirePausedForResource(invalidState, error, unavailable))
        {
            return std::nullopt;
        }

        for (const auto& resource : _resources)
        {
            if (resource.kind == DebugResourceKind::Breakpoint && resource.address == request.address
                && resource.breakpointKind == request.kind)
            {
                invalidState = true;
                error        = "that breakpoint is already armed";
                return std::nullopt;
            }
        }

        const bool    hardware = request.kind == DebugBreakpointKind::Hardware;
        std::uint32_t slot     = kNoDebugHardwareSlot;
        if (hardware)
        {
            slot = allocateHardwareSlot();
            if (slot == kNoDebugHardwareSlot)
            {
                invalidState = true;
                error        = "all four hardware debug slots are in use";
                return std::nullopt;
            }
        }

        const auto releaseSlot = [&]()
        {
            if (slot != kNoDebugHardwareSlot)
            {
                releaseHardwareSlot(slot);
            }
        };

        std::vector<std::string> threads;
        if (!_attachedRsp->queryThreads(threads, kRspTimeout, error))
        {
            releaseSlot();
            return std::nullopt;
        }
        if (threads.empty())
        {
            releaseSlot();
            error = "the GDB stub reported no threads";
            return std::nullopt;
        }
        std::string selectedThread = threads.front();
        if (!hardware && isUserModeAddress(request.address))
        {
            if (_inspector == nullptr)
            {
                releaseSlot();
                invalidState = true;
                error        = "the breakpoint service cannot identify the target process";
                return std::nullopt;
            }
            std::string dtbError;
            const auto  dtbs = _inspector->processDtbs(request.pid, dtbError);
            if (!dtbs)
            {
                releaseSlot();
                invalidState = true;
                error = dtbError.empty() ? "process " + std::to_string(request.pid) + " is not available" : dtbError;
                return std::nullopt;
            }
            const auto selection = selectThreadInProcess(
                *_attachedRsp, _attachedDescription, threads, dtbs->dtb, dtbs->dtbUser, selectedThread, error);
            if (selection == ThreadSelection::Error)
            {
                releaseSlot();
                return std::nullopt;
            }
            if (selection == ThreadSelection::NotInProcess)
            {
                releaseSlot();
                invalidState = true;
                return std::nullopt;
            }
        }
        if (!_attachedRsp->selectThread(selectedThread, kRspTimeout, error))
        {
            releaseSlot();
            return std::nullopt;
        }
        if (!_attachedRsp->insertBreakpoint(request.address, hardware, kRspTimeout, error))
        {
            releaseSlot();
            return std::nullopt;
        }
        _attachedRsp->drainPending();

        ArmedResource resource;
        resource.id             = _nextResourceId++;
        resource.kind           = DebugResourceKind::Breakpoint;
        resource.breakpointKind = request.kind;
        resource.pid            = request.pid;
        resource.address        = request.address;
        resource.slot           = slot;
        resource.threadId       = selectedThread;
        _resources.push_back(resource);

        return DebugBreakpointArmResponse {resource.id, resource.pid, resource.address, resource.breakpointKind, slot};
    }

    std::optional<DebugWatchArmResponse> DebugController::armWatch(std::uint64_t               sessionId,
                                                                   const DebugWatchArmRequest& request,
                                                                   bool&                       invalidState,
                                                                   bool&                       busy,
                                                                   std::string&                error,
                                                                   bool&                       unavailable)
    {
        busy         = false;
        invalidState = false;
        unavailable  = false;
        std::unique_lock lock {_operationMutex, std::try_to_lock};
        if (!lock.owns_lock())
        {
            busy  = true;
            error = "another debug operation is in progress";
            return std::nullopt;
        }
        if (!ownsAttachment(sessionId, invalidState, error))
        {
            return std::nullopt;
        }
        if (_resources.size() >= kMaxDebugArmedResources)
        {
            invalidState = true;
            error        = "no more debug resources can be armed";
            return std::nullopt;
        }
        if (!requirePausedForResource(invalidState, error, unavailable))
        {
            return std::nullopt;
        }

        for (const auto& resource : _resources)
        {
            if (resource.kind == DebugResourceKind::Watchpoint && resource.address == request.address)
            {
                invalidState = true;
                error        = "a watchpoint is already armed at that address";
                return std::nullopt;
            }
        }

        const auto slot = allocateHardwareSlot();
        if (slot == kNoDebugHardwareSlot)
        {
            invalidState = true;
            error        = "all four hardware debug slots are in use";
            return std::nullopt;
        }

        std::vector<std::string> threads;
        if (!_attachedRsp->queryThreads(threads, kRspTimeout, error))
        {
            releaseHardwareSlot(slot);
            return std::nullopt;
        }
        if (threads.empty())
        {
            releaseHardwareSlot(slot);
            error = "the GDB stub reported no threads";
            return std::nullopt;
        }
        if (!_attachedRsp->selectThread(threads.front(), kRspTimeout, error))
        {
            releaseHardwareSlot(slot);
            return std::nullopt;
        }
        bool unsupported = false;
        if (!_attachedRsp->insertWatchpoint(
                request.address, request.width, request.access, kRspTimeout, error, unsupported))
        {
            releaseHardwareSlot(slot);
            if (unsupported)
            {
                unavailable = true;
            }
            return std::nullopt;
        }
        _attachedRsp->drainPending();

        ArmedResource resource;
        resource.id       = _nextResourceId++;
        resource.kind     = DebugResourceKind::Watchpoint;
        resource.pid      = request.pid;
        resource.address  = request.address;
        resource.width    = request.width;
        resource.access   = request.access;
        resource.slot     = slot;
        resource.threadId = threads.front();
        _resources.push_back(resource);
        _watchAccess = request.access;
        refreshWatchCount();

        return DebugWatchArmResponse {
            resource.id, resource.pid, resource.address, resource.width, resource.access, slot};
    }

    std::optional<DebugResourceRemoveResponse> DebugController::removeResource(std::uint64_t     sessionId,
                                                                               std::uint32_t     id,
                                                                               DebugResourceKind kind,
                                                                               bool&             invalidState,
                                                                               std::string&      error,
                                                                               bool&             unavailable)
    {
        if (!ownsAttachment(sessionId, invalidState, error))
        {
            return std::nullopt;
        }
        if (!requirePausedForResource(invalidState, error, unavailable))
        {
            return std::nullopt;
        }

        const auto found = std::find_if(_resources.begin(),
                                        _resources.end(),
                                        [id, kind](const ArmedResource& resource)
                                        {
                                            return resource.id == id && resource.kind == kind;
                                        });
        if (found == _resources.end())
        {
            return DebugResourceRemoveResponse {id, false};
        }

        _attachedRsp->drainPending();
        std::string removeError;
        bool        removed = false;
        if (kind == DebugResourceKind::Watchpoint)
        {
            removed =
                _attachedRsp->removeWatchpoint(found->address, found->width, found->access, kRspTimeout, removeError);
            if (!removed)
            {
                _attachedRsp->drainPending();
                removed = _attachedRsp->removeWatchpoint(
                    found->address, found->width, found->access, kRspTimeout, removeError);
            }
        }
        else
        {
            const bool hardware = found->breakpointKind == DebugBreakpointKind::Hardware;
            removed             = _attachedRsp->removeBreakpoint(found->address, hardware, kRspTimeout, removeError);
            if (!removed)
            {
                _attachedRsp->drainPending();
                removed = _attachedRsp->removeBreakpoint(found->address, hardware, kRspTimeout, removeError);
            }
        }
        if (!removed)
        {
            error = removeError.empty() ? "the debug stub could not remove the resource" : removeError;
            return std::nullopt;
        }

        releaseHardwareSlot(found->slot);
        _resources.erase(found);
        refreshWatchCount();
        return DebugResourceRemoveResponse {id, true};
    }

    std::optional<DebugResourceRemoveResponse>
    DebugController::removeBreakpoint(std::uint64_t                       sessionId,
                                      const DebugBreakpointRemoveRequest& request,
                                      bool&                               invalidState,
                                      bool&                               busy,
                                      std::string&                        error,
                                      bool&                               unavailable)
    {
        busy         = false;
        invalidState = false;
        unavailable  = false;
        std::unique_lock lock {_operationMutex, std::try_to_lock};
        if (!lock.owns_lock())
        {
            busy  = true;
            error = "another debug operation is in progress";
            return std::nullopt;
        }
        return removeResource(sessionId, request.id, DebugResourceKind::Breakpoint, invalidState, error, unavailable);
    }

    std::optional<DebugResourceRemoveResponse> DebugController::removeWatch(std::uint64_t                  sessionId,
                                                                            const DebugWatchRemoveRequest& request,
                                                                            bool&                          invalidState,
                                                                            bool&                          busy,
                                                                            std::string&                   error,
                                                                            bool&                          unavailable)
    {
        busy         = false;
        invalidState = false;
        unavailable  = false;
        std::unique_lock lock {_operationMutex, std::try_to_lock};
        if (!lock.owns_lock())
        {
            busy  = true;
            error = "another debug operation is in progress";
            return std::nullopt;
        }
        return removeResource(sessionId, request.id, DebugResourceKind::Watchpoint, invalidState, error, unavailable);
    }

    std::optional<DebugBreakpointListResponse> DebugController::listBreakpoints(
        std::uint64_t sessionId, bool& invalidState, bool& busy, std::string& error, bool& unavailable)
    {
        busy         = false;
        invalidState = false;
        unavailable  = false;
        std::unique_lock lock {_operationMutex, std::try_to_lock};
        if (!lock.owns_lock())
        {
            busy  = true;
            error = "another debug operation is in progress";
            return std::nullopt;
        }
        if (!ownsAttachment(sessionId, invalidState, error))
        {
            return std::nullopt;
        }

        DebugBreakpointListResponse response;
        for (const auto& resource : _resources)
        {
            if (resource.kind == DebugResourceKind::Breakpoint)
            {
                response.breakpoints.push_back(
                    {resource.id, resource.pid, resource.address, resource.breakpointKind, resource.slot});
            }
        }
        return response;
    }

    std::optional<DebugWatchListResponse> DebugController::listWatches(
        std::uint64_t sessionId, bool& invalidState, bool& busy, std::string& error, bool& unavailable)
    {
        busy         = false;
        invalidState = false;
        unavailable  = false;
        std::unique_lock lock {_operationMutex, std::try_to_lock};
        if (!lock.owns_lock())
        {
            busy  = true;
            error = "another debug operation is in progress";
            return std::nullopt;
        }
        if (!ownsAttachment(sessionId, invalidState, error))
        {
            return std::nullopt;
        }

        DebugWatchListResponse response;
        for (const auto& resource : _resources)
        {
            if (resource.kind == DebugResourceKind::Watchpoint)
            {
                response.watches.push_back(
                    {resource.id, resource.pid, resource.address, resource.width, resource.access, resource.slot});
            }
        }
        return response;
    }

    std::optional<DebugContinueResponse> DebugController::continueGuest(
        std::uint64_t sessionId, bool& invalidState, bool& busy, std::string& error, bool& unavailable)
    {
        busy         = false;
        invalidState = false;
        unavailable  = false;
        std::unique_lock lock {_operationMutex, std::try_to_lock};
        if (!lock.owns_lock())
        {
            busy  = true;
            error = "another debug operation is in progress";
            return std::nullopt;
        }
        if (!ownsAttachment(sessionId, invalidState, error))
        {
            return std::nullopt;
        }

        if (!_sessionRunning.load())
        {
            _attachedRsp->drainPending();
            if (!_attachedRsp->continueExecution(kRspTimeout, error))
            {
                return std::nullopt;
            }
            _sessionRunning = true;
        }

        DebugStatus status;
        if (!readStatus(status, error, unavailable))
        {
            return std::nullopt;
        }
        return status;
    }

    void DebugController::clearArmedResources(RspClient* rsp) noexcept
    {
        try
        {
            if (rsp != nullptr)
            {
                rsp->drainPending();
                for (const auto& resource : _resources)
                {
                    std::string removeError;
                    bool        removed = false;
                    if (resource.kind == DebugResourceKind::Watchpoint)
                    {
                        removed = rsp->removeWatchpoint(
                            resource.address, resource.width, resource.access, kRspTimeout, removeError);
                        if (!removed)
                        {
                            rsp->drainPending();
                            removed = rsp->removeWatchpoint(
                                resource.address, resource.width, resource.access, kRspTimeout, removeError);
                        }
                    }
                    else
                    {
                        const bool hardware = resource.breakpointKind == DebugBreakpointKind::Hardware;
                        removed = rsp->removeBreakpoint(resource.address, hardware, kRspTimeout, removeError);
                        if (!removed)
                        {
                            rsp->drainPending();
                            removed = rsp->removeBreakpoint(resource.address, hardware, kRspTimeout, removeError);
                        }
                    }
                    if (!removed)
                    {
                        log::warn("host", "could not remove an armed debug resource during teardown: ", removeError);
                    }
                }
            }
        }
        catch (...)
        {
            log::warn("host", "debug resource teardown failed unexpectedly");
        }
        _resources.clear();
        _usedHardwareSlots = 0;
        _watchCount        = 0;
        _sessionRunning    = false;
    }

    std::optional<DebugWaitEnd> DebugController::wait(const DebugWaitRequest&     request,
                                                      const DebugWaitHitSink&     onHit,
                                                      const DebugWatchAbortCheck& aborted,
                                                      std::uint64_t               sessionId,
                                                      bool&                       invalidState,
                                                      bool&                       busy,
                                                      std::string&                error,
                                                      bool&                       unavailable)
    {
        busy         = false;
        invalidState = false;
        unavailable  = false;
        std::unique_lock lock {_operationMutex, std::try_to_lock};
        if (!lock.owns_lock())
        {
            busy  = true;
            error = "another debug operation is in progress";
            return std::nullopt;
        }
        if (!ownsAttachment(sessionId, invalidState, error))
        {
            return std::nullopt;
        }
        if (_resources.empty())
        {
            invalidState = true;
            error        = "no breakpoints or watchpoints are armed";
            return std::nullopt;
        }

        QmpStatusInfo current;
        if (!qmpQueryStatus(current, error, unavailable))
        {
            return std::nullopt;
        }
        if (current.status == "shutdown" || current.status == "guest-panicked")
        {
            clearArmedResources(_attachedRsp.get());
            invalidState = true;
            error        = "the guest stopped before any armed resource fired";
            return std::nullopt;
        }

        std::vector<std::string> threads;
        if (!_attachedRsp->queryThreads(threads, kRspTimeout, error))
        {
            return std::nullopt;
        }
        if (threads.empty())
        {
            error = "the GDB stub reported no threads";
            return std::nullopt;
        }

        if (!_sessionRunning.load())
        {
            _attachedRsp->drainPending();
            if (!_attachedRsp->continueExecution(kRspTimeout, error))
            {
                return std::nullopt;
            }
            _sessionRunning = true;
        }

        DebugWaitEnd end;
        end.status                 = DebugWatchStatus::Completed;
        std::uint32_t hits         = 0;
        const auto    waitDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds {request.timeoutMs};

        while (hits < request.maxHits)
        {
            if (aborted && aborted())
            {
                end.status = DebugWatchStatus::Cancelled;
                end.error  = "the debug wait was cancelled";
                break;
            }
            if (requestExpired())
            {
                end.status = DebugWatchStatus::Timeout;
                end.error  = "the request deadline expired";
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= waitDeadline)
            {
                end.status = DebugWatchStatus::Timeout;
                end.error  = "the debug wait timed out";
                break;
            }
            const auto slice = std::min(std::chrono::duration_cast<std::chrono::milliseconds>(waitDeadline - now),
                                        std::chrono::milliseconds {100});

            std::string stopPacket;
            const auto  polled = _attachedRsp->pollPacket(stopPacket, slice, error);
            if (polled == RspPollResult::Timeout)
            {
                continue;
            }
            if (polled == RspPollResult::Closed)
            {
                clearArmedResources(nullptr);
                end.status = DebugWatchStatus::Error;
                end.error  = error.empty() ? "the GDB stub closed the connection" : error;
                break;
            }
            if (!stopPacket.empty() && (stopPacket.front() == 'W' || stopPacket.front() == 'X'))
            {
                clearArmedResources(nullptr);
                end.status = DebugWatchStatus::Error;
                end.error  = "the guest stopped before any armed resource fired";
                break;
            }

            bool       stopped = false;
            const auto thread  = stopThreadId(stopPacket, stopped);
            if (!stopped)
            {
                continue;
            }
            if (thread && !thread->empty() && !_attachedRsp->selectThread(*thread, kRspTimeout, error))
            {
                end.status = DebugWatchStatus::Error;
                end.error  = error;
                break;
            }

            const auto           watchAddress = stopWatchAddress(stopPacket);
            const ArmedResource* resource     = nullptr;
            if (watchAddress.has_value())
            {
                const auto found = std::find_if(_resources.begin(),
                                                _resources.end(),
                                                [address = *watchAddress](const ArmedResource& candidate)
                                                {
                                                    return candidate.kind == DebugResourceKind::Watchpoint
                                                        && candidate.address == address;
                                                });
                if (found != _resources.end())
                {
                    resource = &*found;
                }
            }
            else
            {
                std::uint64_t rip = 0;
                if (!readNamedRegister(*_attachedRsp, _attachedDescription, "rip", rip, kRspTimeout, error))
                {
                    end.status = DebugWatchStatus::Error;
                    end.error  = error;
                    break;
                }
                const auto found =
                    std::find_if(_resources.begin(),
                                 _resources.end(),
                                 [rip](const ArmedResource& candidate)
                                 {
                                     return candidate.kind == DebugResourceKind::Breakpoint && candidate.address == rip;
                                 });
                if (found != _resources.end())
                {
                    resource = &*found;
                }
            }

            if (resource == nullptr)
            {
                ++end.skippedCount;
                if (!_attachedRsp->continueExecution(kRspTimeout, error))
                {
                    end.status = DebugWatchStatus::Error;
                    end.error  = error;
                    break;
                }
                _sessionRunning = true;
                continue;
            }

            std::uint32_t index = 0;
            if (thread && !thread->empty())
            {
                const auto found = std::find(threads.begin(), threads.end(), *thread);
                if (found != threads.end())
                {
                    index = static_cast<std::uint32_t>(std::distance(threads.begin(), found));
                }
            }
            auto record = readRecord(*_attachedRsp, _attachedDescription, index, threads[index], error);
            if (!record)
            {
                end.status = DebugWatchStatus::Error;
                end.error  = error;
                break;
            }

            // A matching PC alone does not prove that a hardware breakpoint on a shared user address trapped in
            // the target process. Require the stop's CR3 to match one of the process DTBs; a shadow page table
            // that MemProcFS does not report may still be accepted when the address lies inside a target module,
            // but the hit is marked unverified. Anything else belongs to another process and is skipped.
            bool hitUnverified = false;
            if (resource->kind == DebugResourceKind::Breakpoint
                && resource->breakpointKind == DebugBreakpointKind::Hardware)
            {
                if (_inspector == nullptr)
                {
                    hitUnverified = true;
                }
                else
                {
                    std::string lookupError;
                    const auto  hitDtb = canonicalDtb(record->cr3);
                    const auto  dtbs   = _inspector->processDtbs(resource->pid, lookupError);
                    const bool  verified =
                        dtbs && (hitDtb == canonicalDtb(dtbs->dtb) || hitDtb == canonicalDtb(dtbs->dtbUser));
                    if (!verified)
                    {
                        const bool moduleHit =
                            _inspector->moduleForAddress(resource->pid, resource->address, lookupError).has_value();
                        if (!moduleHit)
                        {
                            ++end.skippedCount;
                            if (!_attachedRsp->continueExecution(kRspTimeout, error))
                            {
                                end.status = DebugWatchStatus::Error;
                                end.error  = error;
                                break;
                            }
                            _sessionRunning = true;
                            continue;
                        }
                        hitUnverified = true;
                    }
                }
            }

            DebugWaitHit hit;
            hit.id             = resource->id;
            hit.kind           = resource->kind;
            hit.breakpointKind = resource->breakpointKind;
            hit.address        = resource->address;
            hit.unverified     = hitUnverified;
            hit.record         = std::move(*record);
            if (_inspector != nullptr)
            {
                resolveWaitHitSymbols(_inspector, resource->pid, hit);
            }

            ++hits;
            end.hitCount    = hits;
            _sessionRunning = false;
            if (onHit && !onHit(hit))
            {
                end.status = DebugWatchStatus::Cancelled;
                end.error  = "the debug wait result could not be delivered";
                break;
            }
            if (hits >= request.maxHits)
            {
                break;
            }
            if (!_attachedRsp->continueExecution(kRspTimeout, error))
            {
                end.status = DebugWatchStatus::Error;
                end.error  = error;
                break;
            }
            _sessionRunning = true;
        }

        end.hitCount = hits;
        return end;
    }

    std::optional<DebugAttachResponse> DebugController::attach(
        std::uint64_t sessionId, bool& invalidState, bool& busy, std::string& error, bool& unavailable)
    {
        busy         = false;
        invalidState = false;
        unavailable  = false;
        std::unique_lock lock {_operationMutex, std::try_to_lock};
        if (!lock.owns_lock())
        {
            busy  = true;
            error = "another debug operation is in progress";
            return std::nullopt;
        }
        std::lock_guard stateLock {_stateMutex};

        if (_ownerSession.load() != 0)
        {
            busy  = true;
            error = "another debug session is already attached";
            return std::nullopt;
        }

        std::string architecture;
        if (!checkQmpArchitecture(architecture, error, unavailable))
        {
            return std::nullopt;
        }

        QmpStatusInfo current;
        if (!qmpQueryStatus(current, error, unavailable))
        {
            return std::nullopt;
        }

        bool pausedByUs = false;
        if (current.status == "running")
        {
            std::string ignored;
            if (!qmpExecute("stop", ignored, error, unavailable))
            {
                return std::nullopt;
            }
            _hostPaused = true;
            pausedByUs  = true;
            log::info("host", "paused the guest for a debug attach");
        }

        // A stub left active without an owner is a stale one from this host; stop it before replacing it.
        if (_stubActive.load())
        {
            std::string ignored;
            bool        ignoredUnavailable = false;
            if (!stopStub(ignored, ignoredUnavailable))
            {
                error       = ignored.empty() ? "the previous GDB stub could not be stopped" : ignored;
                unavailable = ignoredUnavailable;
                if (pausedByUs)
                {
                    std::string resume;
                    if (!qmpExecute("cont", resume, ignored, ignoredUnavailable))
                    {
                        log::warn("host", "could not restore the running guest after a failed attach: ", ignored);
                    }
                    _hostPaused = false;
                }
                return std::nullopt;
            }
        }

        const auto failAttach = [&](bool startedStub, bool guestPaused, std::string& failure)
        {
            std::string ignored;
            bool        ignoredUnavailable = false;
            if (startedStub && _stubActive.load() && !stopStub(ignored, ignoredUnavailable))
            {
                log::warn("host", "could not stop the GDB stub after a failed attach: ", ignored);
            }
            std::error_code removeError;
            std::filesystem::remove(_stubSocket, removeError);
            if (guestPaused)
            {
                std::string resume;
                if (!qmpExecute("cont", resume, ignored, ignoredUnavailable))
                {
                    log::warn("host", "could not restore the running guest after a failed attach: ", ignored);
                }
                _hostPaused = false;
            }
            if (failure.empty())
            {
                failure = "debug attach failed";
            }
        };

        bool active = false;
        if (!hasActiveStub(active, error, unavailable))
        {
            failAttach(false, pausedByUs, error);
            return std::nullopt;
        }
        if (active)
        {
            invalidState = true;
            error        = "another GDB stub owns the debug channel; run debug-detach first";
            failAttach(false, pausedByUs, error);
            return std::nullopt;
        }

        auto        rsp = makeRsp();
        RspEndpoint endpoint;
        if (!startStub(*rsp, endpoint, error, unavailable))
        {
            failAttach(true, pausedByUs, error);
            return std::nullopt;
        }

        if (!rsp->negotiate(kRspTimeout, error))
        {
            failAttach(true, pausedByUs, error);
            return std::nullopt;
        }

        RspTargetDescription description;
        if (!rsp->readTargetDescription(description, kRspTimeout, error))
        {
            failAttach(true, pausedByUs, error);
            return std::nullopt;
        }
        if (architecture.empty())
        {
            architecture = architectureFromTargetDescription(description);
            if (!isSupportedArchitecture(architecture))
            {
                rejectArchitecture(architecture, true, error, unavailable);
                failAttach(true, pausedByUs, error);
                return std::nullopt;
            }
        }

        _attachedRsp          = std::move(rsp);
        _attachedEndpoint     = endpoint;
        _attachedDescription  = std::move(description);
        _attachedArchitecture = architecture;
        _attachPaused         = pausedByUs;
        _ownerSession         = sessionId;
        _resources.clear();
        _usedHardwareSlots = 0;
        _watchCount        = 0;
        _sessionRunning    = false;

        DebugAttachResponse response;
        response.guestPaused = pausedByUs;
        response.stubMode    = endpoint.kind == RspEndpoint::Kind::Unix ? "unix" : "tcp";
        if (!readStatus(response.status, error, unavailable))
        {
            _attachedRsp.reset();
            _attachedArchitecture.clear();
            _attachedDescription = {};
            _ownerSession        = 0;
            _attachPaused        = false;
            failAttach(true, pausedByUs, error);
            return std::nullopt;
        }

        log::info("host", "attached the debugger for session ", sessionId, " with a ", response.stubMode, " stub");
        return response;
    }

    std::optional<DebugDetachResponse>
    DebugController::detach(std::uint64_t sessionId, bool& busy, std::string& error, bool& unavailable)
    {
        busy        = false;
        unavailable = false;
        std::unique_lock lock {_operationMutex, std::try_to_lock};
        if (!lock.owns_lock())
        {
            busy  = true;
            error = "another debug operation is in progress";
            return std::nullopt;
        }
        std::lock_guard stateLock {_stateMutex};

        const bool wasAttached  = _ownerSession.load() != 0;
        const bool attachPaused = _attachPaused;

        QmpStatusInfo current;
        if (!qmpQueryStatus(current, error, unavailable))
        {
            return std::nullopt;
        }

        bool active = false;
        if (!hasActiveStub(active, error, unavailable))
        {
            return std::nullopt;
        }

        const bool          stubOwned = _stubActive.load();
        DebugDetachResponse response;
        response.detached    = wasAttached;
        response.foreignStub = active && !stubOwned;

        // A running guest cannot have its debug registers changed. Stop it only when some resource still has
        // to be removed, then put it back the way the caller found it.
        const bool guestWasRunning    = _sessionRunning.load() || current.status == "running";
        bool       stoppedForTeardown = false;
        if (!_resources.empty() && guestWasRunning)
        {
            std::string ignored;
            if (qmpExecute("stop", ignored, error, unavailable))
            {
                stoppedForTeardown = true;
            }
            else
            {
                log::warn("host", "could not pause the guest to remove debug resources: ", error);
            }
        }
        clearArmedResources(_attachedRsp.get());

        if (_attachedRsp)
        {
            _attachedRsp->disconnect();
            _attachedRsp.reset();
        }

        if (active)
        {
            std::string reply;
            if (!qmpHumanMonitor("gdbserver none", reply, error, unavailable))
            {
                return std::nullopt;
            }
            response.stubStopped = true;
            log::info("host", response.foreignStub ? "stopped a foreign GDB stub" : "stopped the GDB stub");
        }

        _stubActive = false;
        _watchCount = 0;
        std::error_code removeError;
        std::filesystem::remove(_stubSocket, removeError);

        _attachedArchitecture.clear();
        _attachedDescription = {};
        _ownerSession        = 0;
        _attachPaused        = false;
        _sessionRunning      = false;

        // Resume a guest this detach had to stop, or a pause the attachment made. A guest that was already
        // paused before the attach stays paused, and a pause requested by debug-stop is left for the client
        // that asked for it.
        bool resume = false;
        if (guestWasRunning)
        {
            resume = stoppedForTeardown;
        }
        else
        {
            resume = wasAttached ? attachPaused : _hostPaused.load();
        }
        if (resume)
        {
            std::string ignored;
            if (!qmpExecute("cont", ignored, error, unavailable))
            {
                return std::nullopt;
            }
            response.guestResumed = true;
            _hostPaused           = false;
            log::info("host", "resumed the guest after detaching the debug stub");
        }

        DebugStatus status;
        if (!readStatus(status, error, unavailable))
        {
            return std::nullopt;
        }
        response.status = status;
        log::info("host", "detached the debugger for session ", sessionId);
        return response;
    }

    void DebugController::detachSession(std::uint64_t sessionId) noexcept
    {
        try
        {
            if (_ownerSession.load() != sessionId)
            {
                return;
            }
            bool        busy        = false;
            bool        unavailable = false;
            std::string error;
            if (!detach(sessionId, busy, error, unavailable))
            {
                log::warn("host", "could not detach the debugger for a closing session: ", error);
            }
        }
        catch (...)
        {
            log::warn("host", "debug detach for a closing session failed unexpectedly");
        }
    }

    void DebugController::shutdown() noexcept
    {
        try
        {
            std::unique_lock lock {_operationMutex, std::try_to_lock};
            if (!lock.owns_lock())
            {
                log::warn("host", "a debug operation is still in progress during shutdown");
                return;
            }

            std::string     error;
            bool            unavailable = false;
            std::error_code removeError;
            clearArmedResources(_attachedRsp.get());
            if (_attachedRsp)
            {
                _attachedRsp->disconnect();
                _attachedRsp.reset();
            }
            _ownerSession = 0;
            _attachPaused = false;
            _attachedArchitecture.clear();
            _attachedDescription = {};
            if (!_stubActive.load() && !_hostPaused.load())
            {
                std::filesystem::remove(_stubSocket, removeError);
                std::lock_guard qmpLock {_qmpMutex};
                _qmp.reset();
                return;
            }

            if (_stubActive.load() && !stopStub(error, unavailable))
            {
                log::warn("host", "could not stop the GDB stub during shutdown: ", error);
            }
            std::filesystem::remove(_stubSocket, removeError);

            if (_hostPaused.load())
            {
                QmpStatusInfo current;
                if (qmpQueryStatus(current, error, unavailable) && current.status != "running")
                {
                    std::string ignored;
                    if (!qmpExecute("cont", ignored, error, unavailable))
                    {
                        log::warn("host", "could not resume the guest during shutdown: ", error);
                    }
                }
                _hostPaused = false;
            }

            std::lock_guard qmpLock {_qmpMutex};
            _qmp.reset();
        }
        catch (...)
        {
            log::warn("host", "debug teardown during shutdown failed unexpectedly");
        }
    }
} // namespace mpfshost::debug
