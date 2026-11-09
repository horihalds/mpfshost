#ifndef MPFSHOST_HOST_VMM_HANDLE_HPP
#define MPFSHOST_HOST_VMM_HANDLE_HPP
#pragma once

#include "core/debug/debug_controller.hpp"
#include "core/server/backend.hpp"
#include "core/server/config_provider.hpp"
#include "core/server/debug_provider.hpp"
#include "core/server/handle_provider.hpp"
#include "core/server/heap_alloc_provider.hpp"
#include "core/server/heap_provider.hpp"
#include "core/server/kernel_device_provider.hpp"
#include "core/server/kernel_driver_provider.hpp"
#include "core/server/kernel_object_provider.hpp"
#include "core/server/memory_map_provider.hpp"
#include "core/server/memory_provider.hpp"
#include "core/server/memory_search_provider.hpp"
#include "core/server/module_lookup_provider.hpp"
#include "core/server/module_provider.hpp"
#include "core/server/module_table_provider.hpp"
#include "core/server/network_provider.hpp"
#include "core/server/page_provider.hpp"
#include "core/server/pdb_provider.hpp"
#include "core/server/pfn_provider.hpp"
#include "core/server/physical_memory_provider.hpp"
#include "core/server/pool_provider.hpp"
#include "core/server/process_info_provider.hpp"
#include "core/server/process_provider.hpp"
#include "core/server/pte_provider.hpp"
#include "core/server/scatter_provider.hpp"
#include "core/server/service_provider.hpp"
#include "core/server/thread_provider.hpp"
#include "core/server/unloaded_module_provider.hpp"
#include "core/server/user_provider.hpp"
#include "core/server/vad_ex_provider.hpp"
#include "core/server/vfs_provider.hpp"
#include "core/server/virtual_query_provider.hpp"
#include "core/server/vm_provider.hpp"
#include "host/vmm_lookup_cache.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "vmmdll.h"

namespace mpfshost
{
    class VmmHandle : public VmHandle,
                      public VmProvider,
                      public PhysicalMemoryProvider,
                      public ProcessProvider,
                      public ProcessInfoProvider,
                      public MemoryProvider,
                      public ScatterProvider,
                      public PageReadProvider,
                      public PrefetchProvider,
                      public ModuleProvider,
                      public ModuleLookupProvider,
                      public PdbProvider,
                      public MemoryMapProvider,
                      public VadExProvider,
                      public ThreadProvider,
                      public HandleProvider,
                      public ModuleTableProvider,
                      public UnloadedModuleProvider,
                      public ServiceProvider,
                      public UserProvider,
                      public NetworkProvider,
                      public HeapProvider,
                      public HeapAllocProvider,
                      public KernelDriverProvider,
                      public KernelDeviceProvider,
                      public KernelObjectProvider,
                      public PteProvider,
                      public PoolProvider,
                      public PfnProvider,
                      public VfsProvider,
                      public VirtualQueryProvider,
                      public MemorySearchProvider,
                      public ConfigProvider,
                      public DebugProvider,
                      public debug::DebugInspector
    {
    public:
        VmmHandle(VMM_HANDLE            handle,
                  std::string           targetName,
                  std::filesystem::path qmpSocket,
                  std::filesystem::path stubSocket)
            : _handle {handle}, _targetName {std::move(targetName)}, _lookupCache {handle},
              _debug {std::move(qmpSocket), std::move(stubSocket), {}, {}, this}
        {
        }

        ~VmmHandle() override;

        VmmHandle(const VmmHandle&)            = delete;
        VmmHandle& operator=(const VmmHandle&) = delete;

        [[nodiscard]] std::optional<std::vector<ProcessDescriptor>> listProcesses(const ProcessFilter& filter,
                                                                                  std::string&         error) override;

        [[nodiscard]] std::optional<std::vector<ProcessSummary>> listAllProcesses(const ProcessFilter& filter,
                                                                                  std::string&         error) override;

        [[nodiscard]] std::optional<ProcessInfoResponse>
        processInfo(const ProcessInfoRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<ProcessByDtbResponse>
        processByDtb(const ProcessByDtbRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<std::vector<ModuleDescriptor>> listModules(const ModuleListRequest& request,
                                                                               std::string&             error) override;

        [[nodiscard]] std::optional<SymbolLookupResponse>
        lookupSymbol(const SymbolLookupRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<PdbLoadResponse>
        pdbLoad(const PdbLoadRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<PdbSymbolNameResponse>
        pdbSymbolName(const PdbSymbolNameRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<PdbSymbolAddressResponse>
        pdbSymbolAddress(const PdbSymbolAddressRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<PdbTypeSizeResponse>
        pdbTypeSize(const PdbTypeSizeRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<PdbTypeChildOffsetResponse>
        pdbTypeChildOffset(const PdbTypeChildOffsetRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<ModuleLayoutResponse>
        moduleLayout(const ModuleLayoutRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<ModuleByAddressResponse>
        moduleByAddress(const ModuleByAddressRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<std::vector<MemoryMapRegion>> listMemoryMap(const MemoryMapRequest& request,
                                                                                std::string&            error) override;

        [[nodiscard]] std::optional<std::vector<VadExDescriptor>>
        listVadEx(const VadExListRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<std::vector<ThreadDescriptor>> listThreads(const ThreadListRequest& request,
                                                                               std::string&             error) override;

        [[nodiscard]] std::optional<HandleListResult>
        listHandles(const HandleListRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<ExportListResult>
        listExports(const ExportListRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<ImportListResult>
        listImports(const ImportListRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<UnloadedModuleListResult> listUnloadedModules(
            const UnloadedModuleListRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<ServiceListResult>
        listServices(const ServiceListRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<UserListResult>
        listUsers(const UserListRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<NetworkListResult>
        listNetwork(const NetworkListRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<HeapListResult>
        listHeaps(const HeapListRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<HeapAllocListResult>
        listHeapAllocations(const HeapAllocListRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<KernelDriverListResult>
        listKernelDrivers(const KDriverListRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<KernelDeviceListResult>
        listKernelDevices(const KDeviceListRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<KernelObjectListResult>
        listKernelObjects(const KObjectListRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<PteListResult>
        listPte(const PteListRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<PoolListResult>
        listPool(const PoolListRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<PoolTagListResult>
        listPoolTags(const PoolTagListRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<PfnListResult>
        listPfn(const PfnListRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] std::optional<VirtualRegion>
        queryVirtual(const VirtualQueryRequest& request, LookupStatus& status, std::string& error) override;

        [[nodiscard]] bool readPages(std::uint32_t                  pid,
                                     std::span<const std::uint64_t> addresses,
                                     std::vector<PageReadItem>&     out,
                                     std::string&                   error) override;

        [[nodiscard]] bool
        prefetchPages(std::uint32_t pid, std::span<const std::uint64_t> addresses, std::string& error) override;

        [[nodiscard]] bool searchMemory(const MemorySearchRequest&  request,
                                        std::vector<std::uint64_t>& hits,
                                        bool&                       truncated,
                                        std::string&                error) override;

        [[nodiscard]] bool readMemory(std::uint32_t           pid,
                                      std::uint64_t           address,
                                      std::uint32_t           size,
                                      std::vector<std::byte>& out,
                                      std::string&            error) override;
        [[nodiscard]] bool writeMemory(std::uint32_t              pid,
                                       std::uint64_t              address,
                                       std::span<const std::byte> data,
                                       std::string&               error) override;
        [[nodiscard]] bool
        virt2Phys(std::uint32_t pid, std::uint64_t va, std::uint64_t& pa, std::string& error) override;
        [[nodiscard]] std::shared_ptr<ScatterHandle> createScatter(std::uint32_t pid, std::string& error) override;

        [[nodiscard]] std::optional<std::vector<VmDescriptor>> listVms(std::string& error) override;

        [[nodiscard]] std::optional<std::vector<VfsEntry>>
        listVfs(const VfsListRequest& request, LookupStatus& status, std::string& error) override;
        [[nodiscard]] bool
        readVfs(const VfsReadRequest& request, std::vector<std::byte>& out, std::string& error) override;
        [[nodiscard]] bool
        writeVfs(const VfsWriteRequest& request, std::uint32_t& bytesWritten, std::string& error) override;

        [[nodiscard]] bool readPhysical(std::uint32_t           vmIndex,
                                        std::uint64_t           gpa,
                                        std::uint32_t           size,
                                        std::vector<std::byte>& out,
                                        std::string&            error) override;
        [[nodiscard]] bool writePhysical(std::uint32_t              vmIndex,
                                         std::uint64_t              gpa,
                                         std::span<const std::byte> data,
                                         std::string&               error) override;
        [[nodiscard]] bool
        translateGpa(std::uint32_t vmIndex, std::uint64_t gpa, GpaTranslateResponse& out, std::string& error) override;
        [[nodiscard]] bool scatterPhysical(std::uint32_t                    vmIndex,
                                           const std::vector<ScatterEntry>& entries,
                                           std::vector<ScatterResult>&      results,
                                           std::string&                     error) override;

        [[nodiscard]] bool setConfig(const ConfigSetRequest& request, std::string& error) override;

        [[nodiscard]] std::optional<DebugStatus> debugState(bool& busy, std::string& error, bool& unavailable) override;
        [[nodiscard]] std::optional<DebugStatus>
        debugControl(DebugControlAction action, bool& busy, std::string& error, bool& unavailable) override;
        [[nodiscard]] std::optional<std::vector<DebugRegisterRecord>>
                                                                    debugRegisters(const DebugRegistersRequest& request,
                                                                                   bool&                        invalidState,
                                                                                   bool&                        busy,
                                                                                   std::string&                 error,
                                                                                   bool&                        unavailable) override;
        [[nodiscard]] std::optional<std::vector<DebugThreadRecord>> debugThreads(const DebugThreadsRequest& request,
                                                                                 bool&        invalidState,
                                                                                 bool&        busy,
                                                                                 std::string& error,
                                                                                 bool&        unavailable) override;
        [[nodiscard]] std::optional<DebugDisassembleResponse> debugDisassemble(const DebugDisassembleRequest& request,
                                                                               std::string& error) override;
        [[nodiscard]] std::optional<DebugBacktraceResponse>   debugBacktrace(const DebugBacktraceRequest& request,
                                                                             bool&                        invalidState,
                                                                             bool&                        busy,
                                                                             std::string&                 error,
                                                                             bool& unavailable) override;
        [[nodiscard]] std::optional<DebugWatchEnd>            debugWatch(const DebugWatchRequest&    request,
                                                                         const DebugWatchHitSink&    onHit,
                                                                         const DebugWatchAbortCheck& aborted,
                                                                         bool&                       busy,
                                                                         std::string&                error,
                                                                         bool&                       unavailable,
                                                                         bool&                       notFound) override;
        [[nodiscard]] std::optional<DebugBreakpointResponse>  debugBreakpoint(const DebugBreakpointRequest& request,
                                                                              const DebugWatchAbortCheck&   aborted,
                                                                              bool&                         invalidState,
                                                                              bool&                         busy,
                                                                              std::string&                  error,
                                                                              bool&                         unavailable,
                                                                              bool& notFound) override;
        [[nodiscard]] std::optional<DebugStepResponse>        debugStep(const DebugStepRequest& request,
                                                                        bool&                   invalidState,
                                                                        bool&                   busy,
                                                                        std::string&            error,
                                                                        bool&                   unavailable) override;

        [[nodiscard]] std::optional<DebugAttachResponse> debugAttach(
            std::uint64_t sessionId, bool& invalidState, bool& busy, std::string& error, bool& unavailable) override;
        [[nodiscard]] std::optional<DebugDetachResponse>
             debugDetach(std::uint64_t sessionId, bool& busy, std::string& error, bool& unavailable) override;
        void debugDetachSession(std::uint64_t sessionId) noexcept override;

        [[nodiscard]] std::optional<DebugBreakpointArmResponse>
        debugArmBreakpoint(std::uint64_t                    sessionId,
                           const DebugBreakpointArmRequest& request,
                           bool&                            invalidState,
                           bool&                            busy,
                           std::string&                     error,
                           bool&                            unavailable) override;
        [[nodiscard]] std::optional<DebugResourceRemoveResponse>
        debugRemoveBreakpoint(std::uint64_t                       sessionId,
                              const DebugBreakpointRemoveRequest& request,
                              bool&                               invalidState,
                              bool&                               busy,
                              std::string&                        error,
                              bool&                               unavailable) override;
        [[nodiscard]] std::optional<DebugBreakpointListResponse> debugListBreakpoints(
            std::uint64_t sessionId, bool& invalidState, bool& busy, std::string& error, bool& unavailable) override;
        [[nodiscard]] std::optional<DebugWatchArmResponse> debugArmWatch(std::uint64_t               sessionId,
                                                                         const DebugWatchArmRequest& request,
                                                                         bool&                       invalidState,
                                                                         bool&                       busy,
                                                                         std::string&                error,
                                                                         bool& unavailable) override;
        [[nodiscard]] std::optional<DebugResourceRemoveResponse>
                                                            debugRemoveWatch(std::uint64_t                  sessionId,
                                                                             const DebugWatchRemoveRequest& request,
                                                                             bool&                          invalidState,
                                                                             bool&                          busy,
                                                                             std::string&                   error,
                                                                             bool&                          unavailable) override;
        [[nodiscard]] std::optional<DebugWatchListResponse> debugListWatches(
            std::uint64_t sessionId, bool& invalidState, bool& busy, std::string& error, bool& unavailable) override;
        [[nodiscard]] std::optional<DebugContinueResponse> debugContinueGuest(
            std::uint64_t sessionId, bool& invalidState, bool& busy, std::string& error, bool& unavailable) override;
        [[nodiscard]] std::optional<DebugWaitEnd> debugWait(const DebugWaitRequest&     request,
                                                            const DebugWaitHitSink&     onHit,
                                                            const DebugWatchAbortCheck& aborted,
                                                            std::uint64_t               sessionId,
                                                            bool&                       invalidState,
                                                            bool&                       busy,
                                                            std::string&                error,
                                                            bool&                       unavailable) override;

        [[nodiscard]] std::optional<debug::DebugProcessDtbs> processDtbs(std::uint32_t pid,
                                                                         std::string&  error) override;
        [[nodiscard]] std::optional<debug::DebugModuleHit>
        moduleForAddress(std::uint32_t pid, std::uint64_t address, std::string& error) override;
        [[nodiscard]] debug::DebugExportHit
        nearestExport(std::uint32_t pid, const std::string& module, std::uint64_t address, std::string& error) override;
        [[nodiscard]] std::optional<debug::DebugPdbHit>
        pdbSymbol(std::uint32_t pid, const std::string& module, std::uint64_t offset, std::string& error) override;
        [[nodiscard]] debug::DebugSymbolHit resolveSymbol(std::uint32_t      pid,
                                                          const std::string& module,
                                                          const std::string& symbol,
                                                          std::string&       error) override;
        [[nodiscard]] std::optional<debug::DebugGuestThread>
        guestThread(std::uint64_t cr3, std::uint64_t rip, std::uint64_t rsp, std::string& error) override;

    private:
        VMM_HANDLE             _handle;
        std::string            _targetName;
        LookupCache            _lookupCache;
        debug::DebugController _debug;
    };
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_HANDLE_HPP
