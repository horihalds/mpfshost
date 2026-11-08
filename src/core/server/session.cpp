#include "core/server/session.hpp"

#include "core/ipc/message.hpp"
#include "core/log/log.hpp"
#include "core/server/backend.hpp"
#include "core/version.hpp"

#include <algorithm>
#include <atomic>
#include <utility>
#include <vector>

#include <unistd.h>

namespace mpfshost
{
    namespace
    {
        std::uint64_t nextSessionId()
        {
            static std::atomic<std::uint64_t> counter {1};
            return counter.fetch_add(1);
        }

        TargetDescriptor toDescriptor(const VmTarget& target)
        {
            return {target.name, target.qemuPid, target.qmpSocket.string()};
        }
    } // namespace

    Session::Session(int                       fd,
                     std::uint64_t             connectionId,
                     const TargetSource&       targets,
                     VmBackend&                backend,
                     std::chrono::milliseconds requestTimeout,
                     bool                      allowVfsWrite)
        : _connectionId {connectionId}, _category {"session " + std::to_string(connectionId)}, _fd {fd},
          _targets {targets}, _backend {backend}, _requestTimeout {requestTimeout}, _allowVfsWrite {allowVfsWrite}
    {
    }

    Session::~Session()
    {
        log::debug(_category, "session ended");
        _scatters.clear();
        if (_fd >= 0)
        {
            ::close(_fd);
        }
    }

    void Session::run()
    {
        while (true)
        {
            FrameHeader            header;
            std::vector<std::byte> payload;
            std::string            error;
            const auto             status = readFrame(_fd, header, payload, error);
            if (status == FrameReadStatus::Timeout)
            {
                log::info(_category, "closing after the idle timeout: ", error);
                return;
            }
            if (status == FrameReadStatus::Closed)
            {
                return;
            }
            if (status == FrameReadStatus::Rejected)
            {
                const auto code =
                    header.version != kProtocolVersion ? ErrorCode::UnsupportedVersion : ErrorCode::MalformedPayload;
                sendError(header.requestId, code, error);
                return;
            }

            dispatch(header.type, header.requestId, payload);
            if (_writeFailed)
            {
                return;
            }
        }
    }

    void Session::dispatch(MessageType type, std::uint64_t requestId, std::span<const std::byte> payload)
    {
        log::debug(_category, "request ", messageTypeName(type), " id=", requestId);
        RequestDeadlineScope deadlineScope {_deadline};

        if (type == MessageType::CancelRequest)
        {
            handleCancel(requestId, payload);
            return;
        }

        _deadline.restart(_requestTimeout);
        _inFlight         = requestId;
        _framesForRequest = 0;
        _cancelled        = false;

        struct Completion
        {
            Session& session;

            ~Completion()
            {
                session._inFlight         = 0;
                session._framesForRequest = 0;
            }
        } completion {*this};

        switch (type)
        {
        case MessageType::ListTargetsRequest:
            if (!payload.empty())
            {
                sendError(requestId, ErrorCode::MalformedPayload, "list-targets takes no payload");
                return;
            }
            handleListTargets(requestId);
            return;
        case MessageType::CapabilitiesRequest:
            handleCapabilities(requestId, payload);
            return;
        case MessageType::BindRequest:
            handleBind(requestId, payload);
            return;
        case MessageType::ProcessListRequest:
            handleProcessList(requestId, payload);
            return;
        case MessageType::ProcessListAllRequest:
            handleProcessListAll(requestId, payload);
            return;
        case MessageType::ProcessInfoRequest:
            handleProcessInfo(requestId, payload);
            return;
        case MessageType::ProcessByDtbRequest:
            handleProcessByDtb(requestId, payload);
            return;
        case MessageType::ModuleListRequest:
            handleModuleList(requestId, payload);
            return;
        case MessageType::SymbolLookupRequest:
            handleSymbolLookup(requestId, payload);
            return;
        case MessageType::PdbLoadRequest:
            handlePdbLoad(requestId, payload);
            return;
        case MessageType::PdbSymbolNameRequest:
            handlePdbSymbolName(requestId, payload);
            return;
        case MessageType::PdbSymbolAddressRequest:
            handlePdbSymbolAddress(requestId, payload);
            return;
        case MessageType::PdbTypeSizeRequest:
            handlePdbTypeSize(requestId, payload);
            return;
        case MessageType::PdbTypeChildOffsetRequest:
            handlePdbTypeChildOffset(requestId, payload);
            return;
        case MessageType::ModuleLayoutRequest:
            handleModuleLayout(requestId, payload);
            return;
        case MessageType::ModuleByAddressRequest:
            handleModuleByAddress(requestId, payload);
            return;
        case MessageType::MemoryMapRequest:
            handleMemoryMap(requestId, payload);
            return;
        case MessageType::VadExListRequest:
            handleVadExList(requestId, payload);
            return;
        case MessageType::ThreadListRequest:
            handleThreadList(requestId, payload);
            return;
        case MessageType::HandleListRequest:
            handleHandleList(requestId, payload);
            return;
        case MessageType::ExportListRequest:
            handleExportList(requestId, payload);
            return;
        case MessageType::ImportListRequest:
            handleImportList(requestId, payload);
            return;
        case MessageType::UnloadedModuleListRequest:
            handleUnloadedModuleList(requestId, payload);
            return;
        case MessageType::ServiceListRequest:
            handleServiceList(requestId, payload);
            return;
        case MessageType::UserListRequest:
            handleUserList(requestId, payload);
            return;
        case MessageType::NetworkListRequest:
            handleNetworkList(requestId, payload);
            return;
        case MessageType::HeapListRequest:
            handleHeapList(requestId, payload);
            return;
        case MessageType::HeapAllocListRequest:
            handleHeapAllocList(requestId, payload);
            return;
        case MessageType::KDriverListRequest:
            handleKDriverList(requestId, payload);
            return;
        case MessageType::KDeviceListRequest:
            handleKDeviceList(requestId, payload);
            return;
        case MessageType::KObjectListRequest:
            handleKObjectList(requestId, payload);
            return;
        case MessageType::PteListRequest:
            handlePteList(requestId, payload);
            return;
        case MessageType::PoolListRequest:
            handlePoolList(requestId, payload);
            return;
        case MessageType::PoolTagListRequest:
            handlePoolTagList(requestId, payload);
            return;
        case MessageType::PfnListRequest:
            handlePfnList(requestId, payload);
            return;
        case MessageType::ConfigSetRequest:
            handleConfigSet(requestId, payload);
            return;
        case MessageType::VirtualQueryRequest:
            handleVirtualQuery(requestId, payload);
            return;
        case MessageType::MemorySearchRequest:
            handleMemorySearch(requestId, payload);
            return;
        case MessageType::PageReadRequest:
            handlePageRead(requestId, payload);
            return;
        case MessageType::PrefetchRequest:
            handleMemoryPrefetch(requestId, payload);
            return;
        case MessageType::VmListRequest:
            if (!payload.empty())
            {
                sendError(requestId, ErrorCode::MalformedPayload, "vm-list takes no payload");
                return;
            }
            handleVmList(requestId);
            return;
        case MessageType::GpaTranslateRequest:
            handleGpaTranslate(requestId, payload);
            return;
        case MessageType::Virt2PhysRequest:
            handleVirt2Phys(requestId, payload);
            return;
        case MessageType::PhysicalReadRequest:
            handlePhysicalRead(requestId, payload);
            return;
        case MessageType::PhysicalWriteRequest:
            handlePhysicalWrite(requestId, payload);
            return;
        case MessageType::PhysicalScatterRequest:
            handlePhysicalScatter(requestId, payload);
            return;
        case MessageType::MemoryReadRequest:
            handleMemoryRead(requestId, payload);
            return;
        case MessageType::MemoryWriteRequest:
            handleMemoryWrite(requestId, payload);
            return;
        case MessageType::VfsListRequest:
            handleVfsList(requestId, payload);
            return;
        case MessageType::VfsReadRequest:
            handleVfsRead(requestId, payload);
            return;
        case MessageType::VfsWriteRequest:
            handleVfsWrite(requestId, payload);
            return;
        case MessageType::ScatterCreateRequest:
            handleScatterCreate(requestId, payload);
            return;
        case MessageType::ScatterPrepareRequest:
            handleScatterPrepare(requestId, payload);
            return;
        case MessageType::ScatterExecuteRequest:
            handleScatterExecute(requestId, payload);
            return;
        case MessageType::ScatterExecuteReadRequest:
            handleScatterExecuteRead(requestId, payload);
            return;
        case MessageType::ScatterClearRequest:
            handleScatterClear(requestId, payload);
            return;
        case MessageType::ScatterCloseRequest:
            handleScatterClose(requestId, payload);
            return;
        case MessageType::CancelRequest:
            handleCancel(requestId, payload);
            return;
        default:
            sendError(requestId, ErrorCode::UnknownMessage, "unknown message type");
            return;
        }
    }

    void Session::handleListTargets(std::uint64_t requestId)
    {
        const auto targets = _targets.scan();
        log::debug(_category, "target source returned ", targets.size(), " target(s)");
        for (const auto& target : targets)
        {
            log::debug(_category,
                       "target ",
                       target.name,
                       " qemu-pid=",
                       static_cast<unsigned>(target.qemuPid),
                       " qmp=",
                       target.qmpSocket.string());
        }

        ListTargetsResponse response;
        for (const auto& target : targets)
        {
            response.targets.push_back(toDescriptor(target));
        }
        sendPayload(requestId, MessageType::ListTargetsResponse, encodeListTargetsResponse(response));
    }

    void Session::handleCapabilities(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!payload.empty())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "capabilities takes no payload");
            return;
        }

        CapabilitiesResponse response;
        response.serverVersion = std::string(version());
        response.requestTypes  = {
            static_cast<std::uint16_t>(MessageType::ListTargetsRequest),
            static_cast<std::uint16_t>(MessageType::BindRequest),
            static_cast<std::uint16_t>(MessageType::ProcessListRequest),
            static_cast<std::uint16_t>(MessageType::ProcessListAllRequest),
            static_cast<std::uint16_t>(MessageType::ProcessInfoRequest),
            static_cast<std::uint16_t>(MessageType::ProcessByDtbRequest),
            static_cast<std::uint16_t>(MessageType::ModuleListRequest),
            static_cast<std::uint16_t>(MessageType::SymbolLookupRequest),
            static_cast<std::uint16_t>(MessageType::PdbLoadRequest),
            static_cast<std::uint16_t>(MessageType::PdbSymbolNameRequest),
            static_cast<std::uint16_t>(MessageType::PdbSymbolAddressRequest),
            static_cast<std::uint16_t>(MessageType::PdbTypeSizeRequest),
            static_cast<std::uint16_t>(MessageType::PdbTypeChildOffsetRequest),
            static_cast<std::uint16_t>(MessageType::ModuleLayoutRequest),
            static_cast<std::uint16_t>(MessageType::ModuleByAddressRequest),
            static_cast<std::uint16_t>(MessageType::MemoryMapRequest),
            static_cast<std::uint16_t>(MessageType::VadExListRequest),
            static_cast<std::uint16_t>(MessageType::ThreadListRequest),
            static_cast<std::uint16_t>(MessageType::HandleListRequest),
            static_cast<std::uint16_t>(MessageType::ExportListRequest),
            static_cast<std::uint16_t>(MessageType::ImportListRequest),
            static_cast<std::uint16_t>(MessageType::UnloadedModuleListRequest),
            static_cast<std::uint16_t>(MessageType::ServiceListRequest),
            static_cast<std::uint16_t>(MessageType::UserListRequest),
            static_cast<std::uint16_t>(MessageType::NetworkListRequest),
            static_cast<std::uint16_t>(MessageType::HeapListRequest),
            static_cast<std::uint16_t>(MessageType::HeapAllocListRequest),
            static_cast<std::uint16_t>(MessageType::KDriverListRequest),
            static_cast<std::uint16_t>(MessageType::KDeviceListRequest),
            static_cast<std::uint16_t>(MessageType::KObjectListRequest),
            static_cast<std::uint16_t>(MessageType::PteListRequest),
            static_cast<std::uint16_t>(MessageType::PoolListRequest),
            static_cast<std::uint16_t>(MessageType::PoolTagListRequest),
            static_cast<std::uint16_t>(MessageType::ConfigSetRequest),
            static_cast<std::uint16_t>(MessageType::PfnListRequest),
            static_cast<std::uint16_t>(MessageType::VirtualQueryRequest),
            static_cast<std::uint16_t>(MessageType::MemorySearchRequest),
            static_cast<std::uint16_t>(MessageType::PageReadRequest),
            static_cast<std::uint16_t>(MessageType::PrefetchRequest),
            static_cast<std::uint16_t>(MessageType::VmListRequest),
            static_cast<std::uint16_t>(MessageType::GpaTranslateRequest),
            static_cast<std::uint16_t>(MessageType::Virt2PhysRequest),
            static_cast<std::uint16_t>(MessageType::PhysicalReadRequest),
            static_cast<std::uint16_t>(MessageType::PhysicalWriteRequest),
            static_cast<std::uint16_t>(MessageType::PhysicalScatterRequest),
            static_cast<std::uint16_t>(MessageType::MemoryReadRequest),
            static_cast<std::uint16_t>(MessageType::MemoryWriteRequest),
            static_cast<std::uint16_t>(MessageType::VfsListRequest),
            static_cast<std::uint16_t>(MessageType::VfsReadRequest),
            static_cast<std::uint16_t>(MessageType::ScatterCreateRequest),
            static_cast<std::uint16_t>(MessageType::ScatterPrepareRequest),
            static_cast<std::uint16_t>(MessageType::ScatterExecuteRequest),
            static_cast<std::uint16_t>(MessageType::ScatterExecuteReadRequest),
            static_cast<std::uint16_t>(MessageType::ScatterClearRequest),
            static_cast<std::uint16_t>(MessageType::ScatterCloseRequest),
            static_cast<std::uint16_t>(MessageType::CancelRequest),
            static_cast<std::uint16_t>(MessageType::CapabilitiesRequest),
        };
        if (_allowVfsWrite)
        {
            response.requestTypes.push_back(static_cast<std::uint16_t>(MessageType::VfsWriteRequest));
        }
        sendPayload(requestId, MessageType::CapabilitiesResponse, encodeCapabilitiesResponse(response));
    }

    void Session::handleBind(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (_bound.has_value())
        {
            sendError(requestId, ErrorCode::SessionAlreadyBound, "session is already bound to " + _bound->target.name);
            return;
        }

        const auto request = decodeBindRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed bind request");
            return;
        }

        const auto targets = _targets.scan();
        log::debug(_category, "bind candidate ", request->targetName, " from ", targets.size(), " target(s)");
        for (const auto& target : targets)
        {
            log::debug(_category,
                       "target ",
                       target.name,
                       " qemu-pid=",
                       static_cast<unsigned>(target.qemuPid),
                       " qmp=",
                       target.qmpSocket.string());
        }
        const auto match = std::find_if(targets.begin(),
                                        targets.end(),
                                        [&](const VmTarget& target)
                                        {
                                            return target.name == request->targetName;
                                        });
        if (match == targets.end())
        {
            sendError(requestId, ErrorCode::TargetNotFound, "target '" + request->targetName + "' is not available");
            return;
        }

        std::string error;
        auto        handle = _backend.acquire(*match, error);
        if (!handle)
        {
            sendError(requestId, ErrorCode::TargetUnavailable, error.empty() ? "target is unavailable" : error);
            return;
        }

        _bound = BoundTarget {nextSessionId(), *match, std::move(handle)};
        log::info(_category,
                  "bound to target ",
                  match->name,
                  " (pid=",
                  static_cast<unsigned>(match->qemuPid),
                  ", qmp=",
                  match->qmpSocket.string(),
                  ")");
        sendPayload(requestId,
                    MessageType::BindResponse,
                    encodeBindResponse({_bound->sessionId, toDescriptor(_bound->target)}));
    }

    bool Session::requestAborted(std::uint64_t requestId, ErrorCode& code)
    {
        if (requestId != _inFlight)
        {
            return false;
        }

        // A fully buffered cancel is consumed here so the check point also honours it; peeking first keeps
        // a partial frame in the stream untouched.
        const auto buffered = bufferedFrameHeader(_fd);
        if (buffered && buffered->type == MessageType::CancelRequest)
        {
            FrameHeader            header;
            std::vector<std::byte> payload;
            std::string            error;
            if (readFrame(_fd, header, payload, error) != FrameReadStatus::Complete)
            {
                _writeFailed = true;
                code         = ErrorCode::Cancelled;
                return true;
            }
            handleCancel(header.requestId, payload);
        }

        if (_cancelled)
        {
            code = ErrorCode::Cancelled;
            return true;
        }
        if (_deadline.expired())
        {
            code = ErrorCode::Timeout;
            return true;
        }
        return false;
    }

    void Session::handleCancel(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        const auto request = decodeCancelRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "cancel request carries no request id");
            return;
        }

        const auto targetId = request->requestId;
        if (_inFlight == 0 || _inFlight != targetId || _cancelled)
        {
            sendPayload(requestId, MessageType::CancelResponse, encodeCancelResponse({0, targetId}));
            return;
        }

        if (_framesForRequest > 0)
        {
            log::warn(_category, "request ", targetId, " canceled after ", _framesForRequest, " frame(s)");
            _cancelled   = true;
            _writeFailed = true;
            return;
        }

        sendError(targetId, ErrorCode::Cancelled, "request canceled");
        _cancelled = true;
        sendPayload(requestId, MessageType::CancelResponse, encodeCancelResponse({1, targetId}));
    }

    void Session::abortRequest(std::uint64_t requestId, ErrorCode code)
    {
        if (requestId != _inFlight || _cancelled)
        {
            return;
        }
        if (_framesForRequest == 0)
        {
            sendError(requestId, code, code == ErrorCode::Timeout ? "request deadline exceeded" : "request canceled");
            return;
        }
        log::warn(_category,
                  "request ",
                  requestId,
                  " abandoned after ",
                  _framesForRequest,
                  " frame(s): ",
                  errorCodeName(code));
        _writeFailed = true;
    }

    void Session::sendError(std::uint64_t requestId, ErrorCode code, const std::string& message)
    {
        log::warn(_category, "error response code=", errorCodeName(code), " message=", message);
        sendFrame(MessageType::ErrorResponse, requestId, encodeErrorResponse({code, message}));
    }

    void Session::sendPayload(std::uint64_t requestId, MessageType type, const std::vector<std::byte>& payload)
    {
        if (payload.size() > kMaxPayloadLength)
        {
            sendError(
                requestId, ErrorCode::ResultTooLarge, "response exceeds the maximum payload size; narrow the request");
            return;
        }
        sendFrame(type, requestId, payload);
    }

    void Session::sendFrame(MessageType type, std::uint64_t requestId, std::span<const std::byte> payload)
    {
        // The cancelled request already has its error frame, so whatever the handler produces afterwards is
        // deliberately dropped; any other frame for that id would break the one-error-frame rule.
        if (_cancelled && requestId == _inFlight)
        {
            return;
        }

        // A request that has not answered yet may still be turned into its deadline result; once a frame
        // went out the framing rules allow only the rest of the stream or one error frame, never both.
        if (type != MessageType::ErrorResponse && requestId == _inFlight && _framesForRequest == 0
            && _deadline.expired())
        {
            abortRequest(requestId, ErrorCode::Timeout);
            return;
        }

        std::string error;
        if (!writeFrame(_fd, type, requestId, payload, error))
        {
            _writeFailed = true;
        }
        if (requestId == _inFlight)
        {
            ++_framesForRequest;
        }
    }
} // namespace mpfshost
