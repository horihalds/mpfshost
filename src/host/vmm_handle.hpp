#ifndef MPFSHOST_HOST_VMM_HANDLE_HPP
#define MPFSHOST_HOST_VMM_HANDLE_HPP
#pragma once

#include "core/server/backend.hpp"
#include "core/server/config_provider.hpp"
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
                      public ConfigProvider
    {
    public:
        VmmHandle(VMM_HANDLE handle, std::string targetName)
            : _handle {handle}, _targetName {std::move(targetName)}, _lookupCache {handle}
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

    private:
        VMM_HANDLE  _handle;
        std::string _targetName;
        LookupCache _lookupCache;
    };
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_HANDLE_HPP
