#ifndef MPFSHOST_CORE_SERVER_SESSION_HPP
#define MPFSHOST_CORE_SERVER_SESSION_HPP
#pragma once

#include "core/ipc/frame.hpp"
#include "core/ipc/message.hpp"
#include "core/server/request_deadline.hpp"
#include "core/server/scatter_provider.hpp"
#include "core/target/target.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace mpfshost
{
    class TargetSource;
    class VmBackend;
    class VmHandle;

    class Session
    {
    public:
        Session(int                       fd,
                std::uint64_t             connectionId,
                const TargetSource&       targets,
                VmBackend&                backend,
                std::chrono::milliseconds requestTimeout,
                bool                      allowVfsWrite);
        ~Session();

        Session(const Session&)            = delete;
        Session& operator=(const Session&) = delete;

        void run();

    private:
        struct BoundTarget
        {
            std::uint64_t             sessionId {};
            VmTarget                  target;
            std::shared_ptr<VmHandle> handle;
        };

        struct ScatterSession
        {
            std::shared_ptr<ScatterHandle> handle;
            std::uint32_t                  entryCount {};
            std::uint64_t                  readBytes {};
        };

        void               dispatch(MessageType type, std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleListTargets(std::uint64_t requestId);
        void               handleCapabilities(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleBind(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleProcessList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleProcessListAll(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleProcessInfo(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleProcessByDtb(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleVirtualQuery(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleMemorySearch(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handlePageRead(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleMemoryPrefetch(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleModuleList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleSymbolLookup(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handlePdbLoad(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handlePdbSymbolName(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handlePdbSymbolAddress(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handlePdbTypeSize(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handlePdbTypeChildOffset(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleModuleLayout(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleModuleByAddress(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleMemoryMap(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleVadExList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleThreadList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleHandleList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleExportList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleImportList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleUnloadedModuleList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleServiceList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleUserList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleNetworkList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleHeapList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleHeapAllocList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleKDriverList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleKDeviceList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleKObjectList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handlePteList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handlePoolList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handlePoolTagList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handlePfnList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleDebugState(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleDebugControl(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleDebugRegisters(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleDebugThreads(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleDebugWatch(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleDebugBreakpoint(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleDebugStep(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleDebugAttach(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleDebugDetach(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleDebugBreakpointArm(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleDebugBreakpointRemove(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleDebugBreakpointList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleDebugWatchArm(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleDebugWatchRemove(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleDebugWatchList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleDebugContinue(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleDebugWait(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleConfigSet(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleVmList(std::uint64_t requestId);
        void               handleGpaTranslate(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleVirt2Phys(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handlePhysicalRead(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handlePhysicalWrite(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handlePhysicalScatter(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleMemoryRead(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleMemoryWrite(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleVfsList(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleVfsRead(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleVfsWrite(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleScatterCreate(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleScatterPrepare(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleScatterExecute(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleScatterExecuteRead(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleScatterClear(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleScatterClose(std::uint64_t requestId, std::span<const std::byte> payload);
        void               handleCancel(std::uint64_t requestId, std::span<const std::byte> payload);
        // Reports whether the in-flight request must stop before its next frame, naming the reason in code.
        [[nodiscard]] bool requestAborted(std::uint64_t requestId, ErrorCode& code);
        // True when a non-blocking peek sees the client's end of the connection; used by long streams that
        // would otherwise keep the guest paused after the client disappears.
        [[nodiscard]] bool peerGone();
        void               abortRequest(std::uint64_t requestId, ErrorCode code);
        [[nodiscard]] ScatterSession* findScatter(std::uint32_t scatterId);
        [[nodiscard]] bool
        streamScatterResults(std::uint64_t requestId, const std::vector<ScatterResult>& results, MessageType dataType);
        // Lists the partitions of the bound target, reporting the protocol error itself on failure.
        [[nodiscard]] std::optional<std::vector<VmDescriptor>> loadVms(std::uint64_t requestId);
        void sendError(std::uint64_t requestId, ErrorCode code, const std::string& message);
        void sendFrame(MessageType type, std::uint64_t requestId, std::span<const std::byte> payload);
        void sendPayload(std::uint64_t requestId, MessageType type, const std::vector<std::byte>& payload);

        std::uint64_t                                     _connectionId;
        std::string                                       _category;
        int                                               _fd;
        const TargetSource&                               _targets;
        VmBackend&                                        _backend;
        std::optional<BoundTarget>                        _bound;
        // Declared after _bound so the handles are released before the VmHandle they belong to.
        std::unordered_map<std::uint32_t, ScatterSession> _scatters;
        std::uint32_t                                     _nextScatterId {1};
        std::chrono::milliseconds                         _requestTimeout;
        bool                                              _allowVfsWrite {false};
        RequestDeadline                                   _deadline;
        std::uint64_t                                     _inFlight {};
        std::size_t                                       _framesForRequest {};
        bool                                              _cancelled {};
        bool                                              _writeFailed {};
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_SESSION_HPP
