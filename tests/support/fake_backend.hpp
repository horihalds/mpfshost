#ifndef MPFSHOST_TESTS_SUPPORT_FAKE_BACKEND_HPP
#define MPFSHOST_TESTS_SUPPORT_FAKE_BACKEND_HPP
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mpfshost::testing
{
    class FakeTargetSource : public TargetSource
    {
    public:
        void setTargets(std::vector<VmTarget> targets)
        {
            _targets = std::move(targets);
        }

        [[nodiscard]] std::vector<VmTarget> scan() const override
        {
            return _targets;
        }

    private:
        std::vector<VmTarget> _targets;
    };

    struct FakeProcessState
    {
        std::vector<ProcessDescriptor> processes;
        std::vector<ProcessSummary>    allProcesses;
        std::string                    failure;
        std::optional<ProcessFilter>   lastFilter;
        std::optional<ProcessFilter>   lastAllFilter;
    };

    struct FakeProcessInfoState
    {
        ProcessInfoResponse               response;
        std::string                       failure;
        LookupStatus                      status {LookupStatus::Ok};
        std::optional<ProcessInfoRequest> lastRequest;

        ProcessByDtbResponse               dtbResponse;
        std::string                        dtbFailure;
        LookupStatus                       dtbStatus {LookupStatus::Ok};
        std::optional<ProcessByDtbRequest> lastDtbRequest;
    };

    struct FakeVirtualQueryState
    {
        VirtualRegion                      region;
        std::string                        failure;
        LookupStatus                       status {LookupStatus::Ok};
        std::optional<VirtualQueryRequest> lastRequest;
    };

    struct FakePageState
    {
        std::vector<PageReadItem>                 pages;
        std::string                               failure;
        std::string                               prefetchFailure;
        std::optional<std::vector<std::uint64_t>> lastAddresses;
        std::uint32_t                             lastPid {};
    };

    struct FakeConfigState
    {
        std::string                     failure;
        std::optional<ConfigSetRequest> lastRequest;
        std::vector<ConfigSetRequest>   requests;
    };

    struct FakeSearchState
    {
        std::vector<std::uint64_t>         hits;
        bool                               truncated {};
        std::string                        failure;
        std::chrono::milliseconds          delay {};
        std::optional<MemorySearchRequest> lastRequest;
    };

    struct FakeModuleLookupState
    {
        SymbolLookupResponse               symbolResponse;
        std::string                        symbolFailure;
        LookupStatus                       symbolStatus {LookupStatus::Ok};
        std::optional<SymbolLookupRequest> lastSymbolRequest;

        ModuleLayoutResponse               layoutResponse;
        std::string                        layoutFailure;
        LookupStatus                       layoutStatus {LookupStatus::Ok};
        std::optional<ModuleLayoutRequest> lastLayoutRequest;

        ModuleByAddressResponse               addressResponse;
        std::string                           addressFailure;
        LookupStatus                          addressStatus {LookupStatus::Ok};
        std::optional<ModuleByAddressRequest> lastAddressRequest;
    };

    struct FakePdbState
    {
        PdbLoadResponse               loadResponse;
        std::string                   loadFailure;
        LookupStatus                  loadStatus {LookupStatus::Ok};
        std::optional<PdbLoadRequest> lastLoadRequest;

        PdbSymbolNameResponse               nameResponse;
        std::string                         nameFailure;
        LookupStatus                        nameStatus {LookupStatus::Ok};
        std::optional<PdbSymbolNameRequest> lastNameRequest;

        PdbSymbolAddressResponse               addressResponse;
        std::string                            addressFailure;
        LookupStatus                           addressStatus {LookupStatus::Ok};
        std::optional<PdbSymbolAddressRequest> lastAddressRequest;

        PdbTypeSizeResponse               typeSizeResponse;
        std::string                       typeSizeFailure;
        LookupStatus                      typeSizeStatus {LookupStatus::Ok};
        std::optional<PdbTypeSizeRequest> lastTypeSizeRequest;

        PdbTypeChildOffsetResponse               childOffsetResponse;
        std::string                              childOffsetFailure;
        LookupStatus                             childOffsetStatus {LookupStatus::Ok};
        std::optional<PdbTypeChildOffsetRequest> lastChildOffsetRequest;
    };

    struct FakeMemoryState
    {
        std::vector<std::byte>    bytes; // when non-empty, reads return a prefix of it
        std::string               failure;
        std::string               writeFailure;
        bool                      shortRead {};
        std::uint32_t             lastReadPid {};
        std::uint64_t             lastReadAddress {};
        std::uint32_t             lastReadSize {};
        std::uint32_t             lastWritePid {};
        std::uint64_t             lastWriteAddress {};
        std::vector<std::byte>    lastWriteData;
        std::uint64_t             virt2PhysPa {};
        std::string               virt2PhysFailure;
        std::uint32_t             lastVirt2PhysPid {};
        std::uint64_t             lastVirt2PhysVa {};
        std::chrono::milliseconds readDelay {}; // makes request deadlines and cancellation deterministic
    };

    struct FakeVmState
    {
        std::vector<VmDescriptor> vms;
        std::string               failure;
    };

    struct FakePhysicalState
    {
        std::vector<std::byte>     bytes; // when non-empty, reads return a prefix of it
        std::string                failure;
        std::string                writeFailure;
        bool                       shortRead {};
        std::uint32_t              lastReadVmIndex {};
        std::uint64_t              lastReadGpa {};
        std::uint32_t              lastReadSize {};
        std::uint32_t              lastWriteVmIndex {};
        std::uint64_t              lastWriteGpa {};
        std::vector<std::byte>     lastWriteData;
        std::uint32_t              lastTranslateVmIndex {};
        std::uint64_t              lastTranslateGpa {};
        GpaTranslateResponse       translateResponse {0, 0, true, true};
        std::string                translateFailure;
        std::vector<std::uint32_t> failedScatterIndices;
        std::string                scatterFailure;
        std::uint32_t              lastScatterVmIndex {};
        std::vector<ScatterEntry>  lastScatterEntries;
        std::chrono::milliseconds  readDelay {}; // makes request deadlines and cancellation deterministic
    };

    struct FakeScatterState
    {
        std::atomic<int>                 creates {0};
        std::atomic<int>                 liveHandles {0};
        std::atomic<int>                 executeReads {0};
        std::atomic<int>                 clears {0};
        std::vector<ScatterRequestEntry> entries;
        std::string                      createFailure;
        std::string                      prepareFailure;
        std::string                      executeFailure;
        std::string                      executeReadFailure;
        std::string                      clearFailure;
        std::uint32_t                    pid {};
    };

    struct FakeModuleState
    {
        std::vector<ModuleDescriptor>    modules;
        std::string                      failure;
        std::optional<ModuleListRequest> lastRequest;
    };

    struct FakeMapState
    {
        std::vector<MemoryMapRegion>    regions;
        std::string                     failure;
        std::optional<MemoryMapRequest> lastRequest;
    };

    struct FakeVadExState
    {
        std::vector<VadExDescriptor>    entries;
        std::string                     failure;
        LookupStatus                    status {LookupStatus::Ok};
        std::optional<VadExListRequest> lastRequest;
    };

    struct FakeThreadState
    {
        std::vector<ThreadDescriptor>    threads;
        std::string                      failure;
        std::optional<ThreadListRequest> lastRequest;
    };

    struct FakePteState
    {
        std::vector<PteDescriptor>    entries;
        std::uint64_t                 mapEntries {};
        bool                          truncated {};
        std::string                   failure;
        LookupStatus                  status {LookupStatus::Ok};
        std::optional<PteListRequest> lastRequest;
    };

    struct FakePoolState
    {
        std::vector<PoolDescriptor>       entries;
        std::vector<PoolTagDescriptor>    tags;
        std::uint64_t                     mapEntries {};
        std::uint64_t                     mapTags {};
        std::uint64_t                     mapBytes {};
        bool                              truncated {};
        std::string                       failure;
        LookupStatus                      status {LookupStatus::Ok};
        std::optional<PoolListRequest>    lastRequest;
        std::optional<PoolTagListRequest> lastTagRequest;
    };

    struct FakePfnState
    {
        std::vector<PfnDescriptor>    entries;
        std::string                   failure;
        LookupStatus                  status {LookupStatus::Ok};
        std::optional<PfnListRequest> lastRequest;
    };

    struct FakeHandleListState
    {
        std::vector<HandleDescriptor>    handles;
        bool                             truncated {};
        std::string                      failure;
        LookupStatus                     status {LookupStatus::Ok};
        std::optional<HandleListRequest> lastRequest;
    };

    struct FakeModuleTableState
    {
        std::vector<ExportDescriptor>    exports;
        std::vector<ImportDescriptor>    imports;
        bool                             exportsTruncated {};
        bool                             importsTruncated {};
        std::string                      exportFailure;
        std::string                      importFailure;
        LookupStatus                     exportStatus {LookupStatus::Ok};
        LookupStatus                     importStatus {LookupStatus::Ok};
        std::optional<ExportListRequest> lastExportRequest;
        std::optional<ImportListRequest> lastImportRequest;
    };

    struct FakeUnloadedModuleState
    {
        std::vector<UnloadedModuleDescriptor>    modules;
        bool                                     truncated {};
        std::string                              failure;
        LookupStatus                             status {LookupStatus::Ok};
        std::optional<UnloadedModuleListRequest> lastRequest;
    };

    struct FakeServiceState
    {
        std::vector<ServiceDescriptor>    services;
        bool                              truncated {};
        std::string                       failure;
        LookupStatus                      status {LookupStatus::Ok};
        std::optional<ServiceListRequest> lastRequest;
    };

    struct FakeUserState
    {
        std::vector<UserDescriptor>    users;
        bool                           truncated {};
        std::string                    failure;
        LookupStatus                   status {LookupStatus::Ok};
        std::optional<UserListRequest> lastRequest;
    };

    struct FakeNetworkState
    {
        std::vector<NetworkEntryDescriptor> entries;
        bool                                truncated {};
        std::string                         failure;
        LookupStatus                        status {LookupStatus::Ok};
        std::optional<NetworkListRequest>   lastRequest;
    };

    struct FakeHeapState
    {
        std::vector<HeapDescriptor>    heaps;
        bool                           truncated {};
        std::string                    failure;
        LookupStatus                   status {LookupStatus::Ok};
        std::optional<HeapListRequest> lastRequest;
    };

    struct FakeHeapAllocState
    {
        std::vector<HeapAllocDescriptor>    allocations;
        bool                                truncated {};
        std::string                         failure;
        LookupStatus                        status {LookupStatus::Ok};
        std::optional<HeapAllocListRequest> lastRequest;
    };

    struct FakeKernelDriverState
    {
        std::vector<KernelDriverDescriptor> drivers;
        bool                                truncated {};
        std::string                         failure;
        LookupStatus                        status {LookupStatus::Ok};
        std::optional<KDriverListRequest>   lastRequest;
    };

    struct FakeKernelDeviceState
    {
        std::vector<KernelDeviceDescriptor> devices;
        bool                                truncated {};
        std::string                         failure;
        LookupStatus                        status {LookupStatus::Ok};
        std::optional<KDeviceListRequest>   lastRequest;
    };

    struct FakeKernelObjectState
    {
        std::vector<KernelObjectDescriptor> objects;
        bool                                truncated {};
        std::string                         failure;
        LookupStatus                        status {LookupStatus::Ok};
        std::optional<KObjectListRequest>   lastRequest;
    };

    struct FakeVfsState
    {
        std::vector<VfsEntry>                                   entries;
        std::string                                             failure;
        LookupStatus                                            status {LookupStatus::Ok};
        std::optional<VfsListRequest>                           lastListRequest;
        std::unordered_map<std::string, std::vector<std::byte>> files;
        std::string                                             readFailure;
        std::optional<VfsReadRequest>                           lastReadRequest;
        std::string                                             writeFailure;
        std::optional<VfsWriteRequest>                          lastWriteRequest;
    };

    class FakeScatterHandle : public ScatterHandle
    {
    public:
        explicit FakeScatterHandle(std::shared_ptr<FakeScatterState> state) : _state {std::move(state)}
        {
            ++_state->liveHandles;
        }

        ~FakeScatterHandle() override
        {
            --_state->liveHandles;
        }

        FakeScatterHandle(const FakeScatterHandle&)            = delete;
        FakeScatterHandle& operator=(const FakeScatterHandle&) = delete;

        [[nodiscard]] bool prepare(const std::vector<ScatterRequestEntry>& entries, std::string& error) override
        {
            if (!_state->prepareFailure.empty())
            {
                error = _state->prepareFailure;
                return false;
            }
            _state->entries.insert(_state->entries.end(), entries.begin(), entries.end());
            return true;
        }

        [[nodiscard]] bool execute(std::vector<ScatterResult>& results, std::string& error) override
        {
            if (!_state->executeFailure.empty())
            {
                error = _state->executeFailure;
                return false;
            }
            for (const auto& entry : _state->entries)
            {
                ScatterResult result;
                result.address       = entry.address;
                result.requestedSize = entry.size;
                result.ok            = true;
                if (!entry.write)
                {
                    result.data.resize(entry.size);
                    for (std::size_t i = 0; i < result.data.size(); ++i)
                    {
                        result.data[i] = static_cast<std::byte>((entry.address + i) & 0xFF);
                    }
                    result.bytesRead = entry.size;
                }
                results.push_back(std::move(result));
            }
            return true;
        }

        [[nodiscard]] bool executeRead(std::vector<ScatterResult>& results, std::string& error) override
        {
            if (!_state->executeReadFailure.empty())
            {
                error = _state->executeReadFailure;
                return false;
            }
            ++_state->executeReads;
            for (const auto& entry : _state->entries)
            {
                if (entry.write)
                {
                    continue;
                }
                ScatterResult result;
                result.address       = entry.address;
                result.requestedSize = entry.size;
                result.ok            = true;
                result.bytesRead     = entry.size;
                result.data.resize(entry.size);
                for (std::size_t i = 0; i < result.data.size(); ++i)
                {
                    result.data[i] = static_cast<std::byte>((entry.address + i) & 0xFF);
                }
                results.push_back(std::move(result));
            }
            return true;
        }

        [[nodiscard]] bool clear(std::string& error) override
        {
            if (!_state->clearFailure.empty())
            {
                error = _state->clearFailure;
                return false;
            }
            ++_state->clears;
            _state->entries.clear();
            return true;
        }

    private:
        std::shared_ptr<FakeScatterState> _state;
    };

    class FakeHandle : public VmHandle,
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
                       public VfsProvider,
                       public VirtualQueryProvider,
                       public MemorySearchProvider,
                       public PteProvider,
                       public PoolProvider,
                       public PfnProvider,
                       public ConfigProvider
    {
    public:
        FakeHandle(std::string                              name,
                   std::shared_ptr<FakeProcessState>        processState,
                   std::shared_ptr<FakeProcessInfoState>    processInfoState,
                   std::shared_ptr<FakeModuleLookupState>   moduleLookupState,
                   std::shared_ptr<FakePdbState>            pdbState,
                   std::shared_ptr<FakeMemoryState>         memoryState,
                   std::shared_ptr<FakeScatterState>        scatterState,
                   std::shared_ptr<FakeModuleState>         moduleState,
                   std::shared_ptr<FakeMapState>            mapState,
                   std::shared_ptr<FakeVadExState>          vadExState,
                   std::shared_ptr<FakeThreadState>         threadState,
                   std::shared_ptr<FakeVmState>             vmState,
                   std::shared_ptr<FakePhysicalState>       physicalState,
                   std::shared_ptr<FakeVfsState>            vfsState,
                   std::shared_ptr<FakeVirtualQueryState>   virtualQueryState,
                   std::shared_ptr<FakePageState>           pageState,
                   std::shared_ptr<FakeSearchState>         searchState,
                   std::shared_ptr<FakeHandleListState>     handleListState,
                   std::shared_ptr<FakeModuleTableState>    moduleTableState,
                   std::shared_ptr<FakeUnloadedModuleState> unloadedModuleState,
                   std::shared_ptr<FakeServiceState>        serviceState,
                   std::shared_ptr<FakeUserState>           userState,
                   std::shared_ptr<FakeNetworkState>        networkState,
                   std::shared_ptr<FakeHeapState>           heapState,
                   std::shared_ptr<FakeHeapAllocState>      heapAllocState,
                   std::shared_ptr<FakeKernelDriverState>   kernelDriverState,
                   std::shared_ptr<FakeKernelDeviceState>   kernelDeviceState,
                   std::shared_ptr<FakeKernelObjectState>   kernelObjectState,
                   std::shared_ptr<FakePteState>            pteState,
                   std::shared_ptr<FakePoolState>           poolState,
                   std::shared_ptr<FakePfnState>            pfnState,
                   std::shared_ptr<FakeConfigState>         configState)
            : _name {std::move(name)}, _processState {std::move(processState)},
              _processInfoState {std::move(processInfoState)}, _moduleLookupState {std::move(moduleLookupState)},
              _pdbState {std::move(pdbState)}, _memoryState {std::move(memoryState)},
              _scatterState {std::move(scatterState)}, _moduleState {std::move(moduleState)},
              _mapState {std::move(mapState)}, _vadExState {std::move(vadExState)},
              _threadState {std::move(threadState)}, _vmState {std::move(vmState)},
              _physicalState {std::move(physicalState)}, _vfsState {std::move(vfsState)},
              _virtualQueryState {std::move(virtualQueryState)}, _pageState {std::move(pageState)},
              _searchState {std::move(searchState)}, _handleListState {std::move(handleListState)},
              _moduleTableState {std::move(moduleTableState)}, _unloadedModuleState {std::move(unloadedModuleState)},
              _serviceState {std::move(serviceState)}, _userState {std::move(userState)},
              _networkState {std::move(networkState)}, _heapState {std::move(heapState)},
              _heapAllocState {std::move(heapAllocState)}, _kernelDriverState {std::move(kernelDriverState)},
              _kernelDeviceState {std::move(kernelDeviceState)}, _kernelObjectState {std::move(kernelObjectState)},
              _pteState {std::move(pteState)}, _poolState {std::move(poolState)}, _pfnState {std::move(pfnState)},
              _configState {std::move(configState)}
        {
        }

        [[nodiscard]] const std::string& name() const noexcept
        {
            return _name;
        }

        [[nodiscard]] std::optional<std::vector<ProcessDescriptor>> listProcesses(const ProcessFilter& filter,
                                                                                  std::string&         error) override
        {
            _processState->lastFilter = filter;
            if (!_processState->failure.empty())
            {
                error = _processState->failure;
                return std::nullopt;
            }

            std::vector<ProcessDescriptor> matches;
            for (const auto& process : _processState->processes)
            {
                if (matchesProcessFilter(filter, process))
                {
                    matches.push_back(process);
                }
            }
            return matches;
        }

        [[nodiscard]] std::optional<std::vector<ProcessSummary>> listAllProcesses(const ProcessFilter& filter,
                                                                                  std::string&         error) override
        {
            _processState->lastAllFilter = filter;
            if (!_processState->failure.empty())
            {
                error = _processState->failure;
                return std::nullopt;
            }

            std::vector<ProcessSummary> matches;
            for (const auto& summary : _processState->allProcesses)
            {
                if (matchesProcessFilter(filter, summary.process))
                {
                    matches.push_back(summary);
                }
            }
            return matches;
        }

        [[nodiscard]] std::optional<ProcessInfoResponse>
        processInfo(const ProcessInfoRequest& request, LookupStatus& status, std::string& error) override
        {
            _processInfoState->lastRequest = request;
            if (!_processInfoState->failure.empty())
            {
                status = _processInfoState->status;
                error  = _processInfoState->failure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;
            return _processInfoState->response;
        }

        [[nodiscard]] std::optional<VirtualRegion>
        queryVirtual(const VirtualQueryRequest& request, LookupStatus& status, std::string& error) override
        {
            _virtualQueryState->lastRequest = request;
            if (!_virtualQueryState->failure.empty())
            {
                status = _virtualQueryState->status;
                error  = _virtualQueryState->failure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;
            return _virtualQueryState->region;
        }

        [[nodiscard]] bool readPages(std::uint32_t                  pid,
                                     std::span<const std::uint64_t> addresses,
                                     std::vector<PageReadItem>&     out,
                                     std::string&                   error) override
        {
            _pageState->lastPid       = pid;
            _pageState->lastAddresses = std::vector<std::uint64_t>(addresses.begin(), addresses.end());
            if (!_pageState->failure.empty())
            {
                error = _pageState->failure;
                return false;
            }
            out = _pageState->pages;
            return true;
        }

        [[nodiscard]] bool
        prefetchPages(std::uint32_t pid, std::span<const std::uint64_t> addresses, std::string& error) override
        {
            _pageState->lastPid       = pid;
            _pageState->lastAddresses = std::vector<std::uint64_t>(addresses.begin(), addresses.end());
            if (!_pageState->prefetchFailure.empty())
            {
                error = _pageState->prefetchFailure;
                return false;
            }
            return true;
        }

        [[nodiscard]] bool setConfig(const ConfigSetRequest& request, std::string& error) override
        {
            _configState->lastRequest = request;
            _configState->requests.push_back(request);
            if (!_configState->failure.empty())
            {
                error = _configState->failure;
                return false;
            }
            return true;
        }

        [[nodiscard]] bool searchMemory(const MemorySearchRequest&  request,
                                        std::vector<std::uint64_t>& hits,
                                        bool&                       truncated,
                                        std::string&                error) override
        {
            _searchState->lastRequest = request;
            if (_searchState->delay.count() > 0)
            {
                std::this_thread::sleep_for(_searchState->delay);
            }
            if (!_searchState->failure.empty())
            {
                error = _searchState->failure;
                return false;
            }
            hits      = _searchState->hits;
            truncated = _searchState->truncated;
            return true;
        }

        [[nodiscard]] bool readMemory(std::uint32_t           pid,
                                      std::uint64_t           address,
                                      std::uint32_t           size,
                                      std::vector<std::byte>& out,
                                      std::string&            error) override
        {
            _memoryState->lastReadPid     = pid;
            _memoryState->lastReadAddress = address;
            _memoryState->lastReadSize    = size;
            if (_memoryState->readDelay.count() > 0)
            {
                std::this_thread::sleep_for(_memoryState->readDelay);
            }
            if (!_memoryState->failure.empty())
            {
                error = _memoryState->failure;
                return false;
            }

            if (!_memoryState->bytes.empty())
            {
                const auto count = std::min<std::size_t>(size, _memoryState->bytes.size());
                out.assign(_memoryState->bytes.begin(),
                           _memoryState->bytes.begin() + static_cast<std::ptrdiff_t>(count));
            }
            else
            {
                out.resize(size);
                for (std::size_t i = 0; i < out.size(); ++i)
                {
                    out[i] = static_cast<std::byte>((address + i) & 0xFF);
                }
            }
            if (_memoryState->shortRead && !out.empty())
            {
                out.pop_back();
            }
            return true;
        }

        [[nodiscard]] bool writeMemory(std::uint32_t              pid,
                                       std::uint64_t              address,
                                       std::span<const std::byte> data,
                                       std::string&               error) override
        {
            _memoryState->lastWritePid     = pid;
            _memoryState->lastWriteAddress = address;
            _memoryState->lastWriteData.assign(data.begin(), data.end());
            if (!_memoryState->writeFailure.empty())
            {
                error = _memoryState->writeFailure;
                return false;
            }
            return true;
        }

        [[nodiscard]] bool
        virt2Phys(std::uint32_t pid, std::uint64_t va, std::uint64_t& pa, std::string& error) override
        {
            _memoryState->lastVirt2PhysPid = pid;
            _memoryState->lastVirt2PhysVa  = va;
            if (!_memoryState->virt2PhysFailure.empty())
            {
                error = _memoryState->virt2PhysFailure;
                return false;
            }
            pa = _memoryState->virt2PhysPa;
            return true;
        }

        [[nodiscard]] std::shared_ptr<ScatterHandle> createScatter(std::uint32_t pid, std::string& error) override
        {
            if (!_scatterState->createFailure.empty())
            {
                error = _scatterState->createFailure;
                return nullptr;
            }
            _scatterState->pid = pid;
            ++_scatterState->creates;
            return std::make_shared<FakeScatterHandle>(_scatterState);
        }

        [[nodiscard]] std::optional<std::vector<ModuleDescriptor>> listModules(const ModuleListRequest& request,
                                                                               std::string&             error) override
        {
            _moduleState->lastRequest = request;
            if (!_moduleState->failure.empty())
            {
                error = _moduleState->failure;
                return std::nullopt;
            }

            std::vector<ModuleDescriptor> matches;
            for (const auto& module : _moduleState->modules)
            {
                if (matchesModuleFilter(request, module))
                {
                    matches.push_back(module);
                }
            }
            return matches;
        }

        [[nodiscard]] std::optional<ProcessByDtbResponse>
        processByDtb(const ProcessByDtbRequest& request, LookupStatus& status, std::string& error) override
        {
            _processInfoState->lastDtbRequest = request;
            if (!_processInfoState->dtbFailure.empty())
            {
                status = _processInfoState->dtbStatus;
                error  = _processInfoState->dtbFailure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;
            return _processInfoState->dtbResponse;
        }

        [[nodiscard]] std::optional<SymbolLookupResponse>
        lookupSymbol(const SymbolLookupRequest& request, LookupStatus& status, std::string& error) override
        {
            _moduleLookupState->lastSymbolRequest = request;
            if (!_moduleLookupState->symbolFailure.empty())
            {
                status = _moduleLookupState->symbolStatus;
                error  = _moduleLookupState->symbolFailure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;
            return _moduleLookupState->symbolResponse;
        }

        [[nodiscard]] std::optional<ModuleLayoutResponse>
        moduleLayout(const ModuleLayoutRequest& request, LookupStatus& status, std::string& error) override
        {
            _moduleLookupState->lastLayoutRequest = request;
            if (!_moduleLookupState->layoutFailure.empty())
            {
                status = _moduleLookupState->layoutStatus;
                error  = _moduleLookupState->layoutFailure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;
            return _moduleLookupState->layoutResponse;
        }

        [[nodiscard]] std::optional<ModuleByAddressResponse>
        moduleByAddress(const ModuleByAddressRequest& request, LookupStatus& status, std::string& error) override
        {
            _moduleLookupState->lastAddressRequest = request;
            if (!_moduleLookupState->addressFailure.empty())
            {
                status = _moduleLookupState->addressStatus;
                error  = _moduleLookupState->addressFailure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;
            return _moduleLookupState->addressResponse;
        }

        [[nodiscard]] std::optional<PdbLoadResponse>
        pdbLoad(const PdbLoadRequest& request, LookupStatus& status, std::string& error) override
        {
            _pdbState->lastLoadRequest = request;
            if (!_pdbState->loadFailure.empty())
            {
                status = _pdbState->loadStatus;
                error  = _pdbState->loadFailure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;
            return _pdbState->loadResponse;
        }

        [[nodiscard]] std::optional<PdbSymbolNameResponse>
        pdbSymbolName(const PdbSymbolNameRequest& request, LookupStatus& status, std::string& error) override
        {
            _pdbState->lastNameRequest = request;
            if (!_pdbState->nameFailure.empty())
            {
                status = _pdbState->nameStatus;
                error  = _pdbState->nameFailure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;
            return _pdbState->nameResponse;
        }

        [[nodiscard]] std::optional<PdbSymbolAddressResponse>
        pdbSymbolAddress(const PdbSymbolAddressRequest& request, LookupStatus& status, std::string& error) override
        {
            _pdbState->lastAddressRequest = request;
            if (!_pdbState->addressFailure.empty())
            {
                status = _pdbState->addressStatus;
                error  = _pdbState->addressFailure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;
            return _pdbState->addressResponse;
        }

        [[nodiscard]] std::optional<PdbTypeSizeResponse>
        pdbTypeSize(const PdbTypeSizeRequest& request, LookupStatus& status, std::string& error) override
        {
            _pdbState->lastTypeSizeRequest = request;
            if (!_pdbState->typeSizeFailure.empty())
            {
                status = _pdbState->typeSizeStatus;
                error  = _pdbState->typeSizeFailure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;
            return _pdbState->typeSizeResponse;
        }

        [[nodiscard]] std::optional<PdbTypeChildOffsetResponse>
        pdbTypeChildOffset(const PdbTypeChildOffsetRequest& request, LookupStatus& status, std::string& error) override
        {
            _pdbState->lastChildOffsetRequest = request;
            if (!_pdbState->childOffsetFailure.empty())
            {
                status = _pdbState->childOffsetStatus;
                error  = _pdbState->childOffsetFailure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;
            return _pdbState->childOffsetResponse;
        }

        [[nodiscard]] std::optional<std::vector<MemoryMapRegion>> listMemoryMap(const MemoryMapRequest& request,
                                                                                std::string&            error) override
        {
            _mapState->lastRequest = request;
            if (!_mapState->failure.empty())
            {
                error = _mapState->failure;
                return std::nullopt;
            }
            return _mapState->regions;
        }

        [[nodiscard]] std::optional<std::vector<VadExDescriptor>>
        listVadEx(const VadExListRequest& request, LookupStatus& status, std::string& error) override
        {
            _vadExState->lastRequest = request;
            if (!_vadExState->failure.empty())
            {
                status = _vadExState->status;
                error  = _vadExState->failure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;
            return _vadExState->entries;
        }

        [[nodiscard]] std::optional<std::vector<ThreadDescriptor>> listThreads(const ThreadListRequest& request,
                                                                               std::string&             error) override
        {
            _threadState->lastRequest = request;
            if (!_threadState->failure.empty())
            {
                error = _threadState->failure;
                return std::nullopt;
            }

            std::vector<ThreadDescriptor> matches;
            for (const auto& thread : _threadState->threads)
            {
                if (matchesThreadFilter(request, thread))
                {
                    matches.push_back(thread);
                }
            }
            return matches;
        }

        [[nodiscard]] std::optional<HandleListResult>
        listHandles(const HandleListRequest& request, LookupStatus& status, std::string& error) override
        {
            _handleListState->lastRequest = request;
            if (!_handleListState->failure.empty())
            {
                status = _handleListState->status;
                error  = _handleListState->failure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;

            HandleListResult result;
            result.truncated = _handleListState->truncated;
            for (const auto& handle : _handleListState->handles)
            {
                if (matchesHandleFilter(request, handle))
                {
                    result.handles.push_back(handle);
                }
            }
            return result;
        }

        [[nodiscard]] std::optional<ExportListResult>
        listExports(const ExportListRequest& request, LookupStatus& status, std::string& error) override
        {
            _moduleTableState->lastExportRequest = request;
            if (!_moduleTableState->exportFailure.empty())
            {
                status = _moduleTableState->exportStatus;
                error  = _moduleTableState->exportFailure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;
            return ExportListResult {_moduleTableState->exports, _moduleTableState->exportsTruncated};
        }

        [[nodiscard]] std::optional<ImportListResult>
        listImports(const ImportListRequest& request, LookupStatus& status, std::string& error) override
        {
            _moduleTableState->lastImportRequest = request;
            if (!_moduleTableState->importFailure.empty())
            {
                status = _moduleTableState->importStatus;
                error  = _moduleTableState->importFailure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;
            return ImportListResult {_moduleTableState->imports, _moduleTableState->importsTruncated};
        }

        [[nodiscard]] std::optional<UnloadedModuleListResult>
        listUnloadedModules(const UnloadedModuleListRequest& request, LookupStatus& status, std::string& error) override
        {
            _unloadedModuleState->lastRequest = request;
            if (!_unloadedModuleState->failure.empty())
            {
                status = _unloadedModuleState->status;
                error  = _unloadedModuleState->failure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;

            UnloadedModuleListResult result;
            result.truncated = _unloadedModuleState->truncated;
            for (const auto& module : _unloadedModuleState->modules)
            {
                if (matchesUnloadedModuleFilter(request, module))
                {
                    result.modules.push_back(module);
                }
            }
            return result;
        }

        [[nodiscard]] std::optional<ServiceListResult>
        listServices(const ServiceListRequest& request, LookupStatus& status, std::string& error) override
        {
            _serviceState->lastRequest = request;
            if (!_serviceState->failure.empty())
            {
                status = _serviceState->status;
                error  = _serviceState->failure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;

            ServiceListResult result;
            result.truncated = _serviceState->truncated;
            for (const auto& service : _serviceState->services)
            {
                if (matchesServiceFilter(request, service))
                {
                    result.services.push_back(service);
                }
            }
            return result;
        }

        [[nodiscard]] std::optional<UserListResult>
        listUsers(const UserListRequest& request, LookupStatus& status, std::string& error) override
        {
            _userState->lastRequest = request;
            if (!_userState->failure.empty())
            {
                status = _userState->status;
                error  = _userState->failure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;

            UserListResult result;
            result.truncated = _userState->truncated;
            for (const auto& user : _userState->users)
            {
                if (matchesUserFilter(request, user))
                {
                    result.users.push_back(user);
                }
            }
            return result;
        }

        [[nodiscard]] std::optional<NetworkListResult>
        listNetwork(const NetworkListRequest& request, LookupStatus& status, std::string& error) override
        {
            _networkState->lastRequest = request;
            if (!_networkState->failure.empty())
            {
                status = _networkState->status;
                error  = _networkState->failure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;

            NetworkListResult result;
            result.truncated = _networkState->truncated;
            for (const auto& entry : _networkState->entries)
            {
                if (matchesNetworkFilter(request, entry))
                {
                    result.entries.push_back(entry);
                }
            }
            return result;
        }

        [[nodiscard]] std::optional<HeapListResult>
        listHeaps(const HeapListRequest& request, LookupStatus& status, std::string& error) override
        {
            _heapState->lastRequest = request;
            if (!_heapState->failure.empty())
            {
                status = _heapState->status;
                error  = _heapState->failure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;

            HeapListResult result;
            result.truncated = _heapState->truncated;
            result.heaps     = _heapState->heaps;
            return result;
        }

        [[nodiscard]] std::optional<HeapAllocListResult>
        listHeapAllocations(const HeapAllocListRequest& request, LookupStatus& status, std::string& error) override
        {
            _heapAllocState->lastRequest = request;
            if (!_heapAllocState->failure.empty())
            {
                status = _heapAllocState->status;
                error  = _heapAllocState->failure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;

            HeapAllocListResult result;
            result.truncated   = _heapAllocState->truncated;
            result.allocations = _heapAllocState->allocations;
            return result;
        }

        [[nodiscard]] std::optional<KernelDriverListResult>
        listKernelDrivers(const KDriverListRequest& request, LookupStatus& status, std::string& error) override
        {
            _kernelDriverState->lastRequest = request;
            if (!_kernelDriverState->failure.empty())
            {
                status = _kernelDriverState->status;
                error  = _kernelDriverState->failure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;

            KernelDriverListResult result;
            result.truncated = _kernelDriverState->truncated;
            for (const auto& driver : _kernelDriverState->drivers)
            {
                if (matchesKernelDriverFilter(request, driver))
                {
                    result.drivers.push_back(driver);
                }
            }
            return result;
        }

        [[nodiscard]] std::optional<KernelDeviceListResult>
        listKernelDevices(const KDeviceListRequest& request, LookupStatus& status, std::string& error) override
        {
            _kernelDeviceState->lastRequest = request;
            if (!_kernelDeviceState->failure.empty())
            {
                status = _kernelDeviceState->status;
                error  = _kernelDeviceState->failure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;

            KernelDeviceListResult result;
            result.truncated = _kernelDeviceState->truncated;
            for (const auto& device : _kernelDeviceState->devices)
            {
                if (matchesKernelDeviceFilter(request, device))
                {
                    result.devices.push_back(device);
                }
            }
            return result;
        }

        [[nodiscard]] std::optional<KernelObjectListResult>
        listKernelObjects(const KObjectListRequest& request, LookupStatus& status, std::string& error) override
        {
            _kernelObjectState->lastRequest = request;
            if (!_kernelObjectState->failure.empty())
            {
                status = _kernelObjectState->status;
                error  = _kernelObjectState->failure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;

            KernelObjectListResult result;
            result.truncated = _kernelObjectState->truncated;
            for (const auto& object : _kernelObjectState->objects)
            {
                if (matchesKernelObjectFilter(request, object))
                {
                    result.objects.push_back(object);
                }
            }
            return result;
        }

        [[nodiscard]] std::optional<PteListResult>
        listPte(const PteListRequest& request, LookupStatus& status, std::string& error) override
        {
            _pteState->lastRequest = request;
            if (!_pteState->failure.empty())
            {
                status = _pteState->status;
                error  = _pteState->failure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;

            PteListResult result;
            result.mapEntries = _pteState->mapEntries;
            result.truncated  = _pteState->truncated;
            for (const auto& entry : _pteState->entries)
            {
                if (matchesPteFilter(request, entry))
                {
                    result.entries.push_back(entry);
                }
            }
            return result;
        }

        [[nodiscard]] std::optional<PoolListResult>
        listPool(const PoolListRequest& request, LookupStatus& status, std::string& error) override
        {
            _poolState->lastRequest = request;
            if (!_poolState->failure.empty())
            {
                status = _poolState->status;
                error  = _poolState->failure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;

            PoolListResult result;
            result.mapEntries = _poolState->mapEntries;
            result.mapBytes   = _poolState->mapBytes;
            result.truncated  = _poolState->truncated;
            for (const auto& entry : _poolState->entries)
            {
                if (matchesPoolFilter(request, entry))
                {
                    result.entries.push_back(entry);
                }
            }
            return result;
        }

        [[nodiscard]] std::optional<PoolTagListResult>
        listPoolTags(const PoolTagListRequest& request, LookupStatus& status, std::string& error) override
        {
            _poolState->lastTagRequest = request;
            if (!_poolState->failure.empty())
            {
                status = _poolState->status;
                error  = _poolState->failure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;

            PoolTagListResult result;
            result.mapTags    = _poolState->mapTags;
            result.mapEntries = _poolState->mapEntries;
            result.truncated  = _poolState->truncated;
            for (const auto& entry : _poolState->tags)
            {
                if (matchesPoolTagFilter(request, entry))
                {
                    result.tags.push_back(entry);
                }
            }
            return result;
        }

        [[nodiscard]] std::optional<PfnListResult>
        listPfn(const PfnListRequest& request, LookupStatus& status, std::string& error) override
        {
            _pfnState->lastRequest = request;
            if (!_pfnState->failure.empty())
            {
                status = _pfnState->status;
                error  = _pfnState->failure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;

            PfnListResult result;
            result.entries = _pfnState->entries;
            return result;
        }

        [[nodiscard]] std::optional<std::vector<VfsEntry>>
        listVfs(const VfsListRequest& request, LookupStatus& status, std::string& error) override
        {
            _vfsState->lastListRequest = request;
            if (!_vfsState->failure.empty())
            {
                status = _vfsState->status;
                error  = _vfsState->failure;
                return std::nullopt;
            }
            status = LookupStatus::Ok;

            std::vector<VfsEntry> matches;
            for (const auto& entry : _vfsState->entries)
            {
                if (matchesVfsFilter(request, entry))
                {
                    matches.push_back(entry);
                }
            }
            return matches;
        }

        [[nodiscard]] bool
        readVfs(const VfsReadRequest& request, std::vector<std::byte>& out, std::string& error) override
        {
            _vfsState->lastReadRequest = request;
            if (!_vfsState->readFailure.empty())
            {
                error = _vfsState->readFailure;
                return false;
            }

            const auto file = _vfsState->files.find(request.path);
            if (file == _vfsState->files.end())
            {
                error = "path '" + request.path + "' does not exist";
                return false;
            }

            const auto& bytes = file->second;
            if (request.offset >= bytes.size())
            {
                out.clear();
                return true;
            }
            const auto available = bytes.size() - static_cast<std::size_t>(request.offset);
            const auto count     = std::min<std::size_t>(request.size, available);
            out.assign(bytes.begin() + static_cast<std::ptrdiff_t>(request.offset),
                       bytes.begin() + static_cast<std::ptrdiff_t>(request.offset + count));
            return true;
        }

        [[nodiscard]] bool
        writeVfs(const VfsWriteRequest& request, std::uint32_t& bytesWritten, std::string& error) override
        {
            _vfsState->lastWriteRequest = request;
            if (!_vfsState->writeFailure.empty())
            {
                error = _vfsState->writeFailure;
                return false;
            }

            auto&      bytes = _vfsState->files[request.path];
            const auto end   = static_cast<std::size_t>(request.offset) + request.data.size();
            if (bytes.size() < end)
            {
                bytes.resize(end, std::byte {0});
            }
            std::copy(
                request.data.begin(), request.data.end(), bytes.begin() + static_cast<std::ptrdiff_t>(request.offset));
            bytesWritten = static_cast<std::uint32_t>(request.data.size());
            return true;
        }

        [[nodiscard]] std::optional<std::vector<VmDescriptor>> listVms(std::string& error) override
        {
            if (!_vmState->failure.empty())
            {
                error = _vmState->failure;
                return std::nullopt;
            }
            return _vmState->vms;
        }

        [[nodiscard]] bool readPhysical(std::uint32_t           vmIndex,
                                        std::uint64_t           gpa,
                                        std::uint32_t           size,
                                        std::vector<std::byte>& out,
                                        std::string&            error) override
        {
            _physicalState->lastReadVmIndex = vmIndex;
            _physicalState->lastReadGpa     = gpa;
            _physicalState->lastReadSize    = size;
            if (_physicalState->readDelay.count() > 0)
            {
                std::this_thread::sleep_for(_physicalState->readDelay);
            }
            if (!_physicalState->failure.empty())
            {
                error = _physicalState->failure;
                return false;
            }

            if (!_physicalState->bytes.empty())
            {
                const auto count = std::min<std::size_t>(size, _physicalState->bytes.size());
                out.assign(_physicalState->bytes.begin(),
                           _physicalState->bytes.begin() + static_cast<std::ptrdiff_t>(count));
            }
            else
            {
                out.resize(size);
                for (std::size_t i = 0; i < out.size(); ++i)
                {
                    out[i] = static_cast<std::byte>((gpa + i) & 0xFF);
                }
            }
            if (_physicalState->shortRead && !out.empty())
            {
                out.pop_back();
            }
            return true;
        }

        [[nodiscard]] bool writePhysical(std::uint32_t              vmIndex,
                                         std::uint64_t              gpa,
                                         std::span<const std::byte> data,
                                         std::string&               error) override
        {
            _physicalState->lastWriteVmIndex = vmIndex;
            _physicalState->lastWriteGpa     = gpa;
            _physicalState->lastWriteData.assign(data.begin(), data.end());
            if (!_physicalState->writeFailure.empty())
            {
                error = _physicalState->writeFailure;
                return false;
            }
            return true;
        }

        [[nodiscard]] bool
        translateGpa(std::uint32_t vmIndex, std::uint64_t gpa, GpaTranslateResponse& out, std::string& error) override
        {
            _physicalState->lastTranslateVmIndex = vmIndex;
            _physicalState->lastTranslateGpa     = gpa;
            if (!_physicalState->translateFailure.empty())
            {
                error = _physicalState->translateFailure;
                return false;
            }
            out = _physicalState->translateResponse;
            return true;
        }

        [[nodiscard]] bool scatterPhysical(std::uint32_t                    vmIndex,
                                           const std::vector<ScatterEntry>& entries,
                                           std::vector<ScatterResult>&      results,
                                           std::string&                     error) override
        {
            _physicalState->lastScatterVmIndex = vmIndex;
            _physicalState->lastScatterEntries = entries;
            if (!_physicalState->scatterFailure.empty())
            {
                error = _physicalState->scatterFailure;
                return false;
            }

            for (std::size_t index = 0; index < entries.size(); ++index)
            {
                const auto& entry = entries[index];

                ScatterResult result;
                result.address       = entry.address;
                result.requestedSize = entry.size;
                const auto failed    = std::find(_physicalState->failedScatterIndices.begin(),
                                                 _physicalState->failedScatterIndices.end(),
                                                 static_cast<std::uint32_t>(index));
                result.ok            = failed == _physicalState->failedScatterIndices.end();
                if (result.ok && entry.kind == kScatterEntryRead)
                {
                    result.data.resize(entry.size);
                    for (std::size_t i = 0; i < result.data.size(); ++i)
                    {
                        result.data[i] = static_cast<std::byte>((entry.address + i) & 0xFF);
                    }
                    result.bytesRead = entry.size;
                }
                results.push_back(std::move(result));
            }
            return true;
        }

    private:
        std::string                              _name;
        std::shared_ptr<FakeProcessState>        _processState;
        std::shared_ptr<FakeProcessInfoState>    _processInfoState;
        std::shared_ptr<FakeModuleLookupState>   _moduleLookupState;
        std::shared_ptr<FakePdbState>            _pdbState;
        std::shared_ptr<FakeMemoryState>         _memoryState;
        std::shared_ptr<FakeScatterState>        _scatterState;
        std::shared_ptr<FakeModuleState>         _moduleState;
        std::shared_ptr<FakeMapState>            _mapState;
        std::shared_ptr<FakeVadExState>          _vadExState;
        std::shared_ptr<FakeThreadState>         _threadState;
        std::shared_ptr<FakeVmState>             _vmState;
        std::shared_ptr<FakePhysicalState>       _physicalState;
        std::shared_ptr<FakeVfsState>            _vfsState;
        std::shared_ptr<FakeVirtualQueryState>   _virtualQueryState;
        std::shared_ptr<FakePageState>           _pageState;
        std::shared_ptr<FakeSearchState>         _searchState;
        std::shared_ptr<FakeHandleListState>     _handleListState;
        std::shared_ptr<FakeModuleTableState>    _moduleTableState;
        std::shared_ptr<FakeUnloadedModuleState> _unloadedModuleState;
        std::shared_ptr<FakeServiceState>        _serviceState;
        std::shared_ptr<FakeUserState>           _userState;
        std::shared_ptr<FakeNetworkState>        _networkState;
        std::shared_ptr<FakeHeapState>           _heapState;
        std::shared_ptr<FakeHeapAllocState>      _heapAllocState;
        std::shared_ptr<FakeKernelDriverState>   _kernelDriverState;
        std::shared_ptr<FakeKernelDeviceState>   _kernelDeviceState;
        std::shared_ptr<FakeKernelObjectState>   _kernelObjectState;
        std::shared_ptr<FakePteState>            _pteState;
        std::shared_ptr<FakePoolState>           _poolState;
        std::shared_ptr<FakePfnState>            _pfnState;
        std::shared_ptr<FakeConfigState>         _configState;
    };

    class BareHandle : public VmHandle
    {
    };

    class FakeBackend : public VmBackend
    {
    public:
        void failWith(std::string message)
        {
            std::lock_guard lock {_mutex};
            _failMessage = std::move(message);
        }

        void clearFailure()
        {
            std::lock_guard lock {_mutex};
            _failMessage.clear();
        }

        void setProcesses(std::vector<ProcessDescriptor> processes)
        {
            _processState->processes = std::move(processes);
        }

        void failProcessesWith(std::string message)
        {
            _processState->failure = std::move(message);
        }

        void setAllProcesses(std::vector<ProcessSummary> processes)
        {
            _processState->allProcesses = std::move(processes);
        }

        void setProcessInfo(ProcessInfoResponse response)
        {
            _processInfoState->response = std::move(response);
        }

        void failProcessInfoWith(std::string message, LookupStatus status = LookupStatus::Failed)
        {
            _processInfoState->failure = std::move(message);
            _processInfoState->status  = status;
        }

        [[nodiscard]] std::shared_ptr<FakeProcessInfoState> processInfoState() const
        {
            return _processInfoState;
        }

        void setProcessByDtbResponse(ProcessByDtbResponse response)
        {
            _processInfoState->dtbResponse = std::move(response);
        }

        void failProcessByDtbWith(std::string message, LookupStatus status = LookupStatus::Failed)
        {
            _processInfoState->dtbFailure = std::move(message);
            _processInfoState->dtbStatus  = status;
        }

        void setSymbolResponse(SymbolLookupResponse response)
        {
            _moduleLookupState->symbolResponse = std::move(response);
        }

        void failSymbolWith(std::string message, LookupStatus status = LookupStatus::Failed)
        {
            _moduleLookupState->symbolFailure = std::move(message);
            _moduleLookupState->symbolStatus  = status;
        }

        [[nodiscard]] std::shared_ptr<FakeModuleLookupState> moduleLookupState() const
        {
            return _moduleLookupState;
        }

        void setPdbLoadResponse(PdbLoadResponse response)
        {
            _pdbState->loadResponse = std::move(response);
        }

        void failPdbLoadWith(std::string message, LookupStatus status = LookupStatus::Failed)
        {
            _pdbState->loadFailure = std::move(message);
            _pdbState->loadStatus  = status;
        }

        void setPdbSymbolNameResponse(PdbSymbolNameResponse response)
        {
            _pdbState->nameResponse = std::move(response);
        }

        void failPdbSymbolNameWith(std::string message, LookupStatus status = LookupStatus::Failed)
        {
            _pdbState->nameFailure = std::move(message);
            _pdbState->nameStatus  = status;
        }

        void setPdbSymbolAddressResponse(PdbSymbolAddressResponse response)
        {
            _pdbState->addressResponse = std::move(response);
        }

        void failPdbSymbolAddressWith(std::string message, LookupStatus status = LookupStatus::Failed)
        {
            _pdbState->addressFailure = std::move(message);
            _pdbState->addressStatus  = status;
        }

        void setPdbTypeSizeResponse(PdbTypeSizeResponse response)
        {
            _pdbState->typeSizeResponse = std::move(response);
        }

        void failPdbTypeSizeWith(std::string message, LookupStatus status = LookupStatus::Failed)
        {
            _pdbState->typeSizeFailure = std::move(message);
            _pdbState->typeSizeStatus  = status;
        }

        void setPdbTypeChildOffsetResponse(PdbTypeChildOffsetResponse response)
        {
            _pdbState->childOffsetResponse = std::move(response);
        }

        void failPdbTypeChildOffsetWith(std::string message, LookupStatus status = LookupStatus::Failed)
        {
            _pdbState->childOffsetFailure = std::move(message);
            _pdbState->childOffsetStatus  = status;
        }

        [[nodiscard]] std::shared_ptr<FakePdbState> pdbState() const
        {
            return _pdbState;
        }

        void setLayoutResponse(ModuleLayoutResponse response)
        {
            _moduleLookupState->layoutResponse = std::move(response);
        }

        void failLayoutWith(std::string message, LookupStatus status = LookupStatus::Failed)
        {
            _moduleLookupState->layoutFailure = std::move(message);
            _moduleLookupState->layoutStatus  = status;
        }

        void setModuleByAddressResponse(ModuleByAddressResponse response)
        {
            _moduleLookupState->addressResponse = std::move(response);
        }

        void failModuleByAddressWith(std::string message, LookupStatus status = LookupStatus::Failed)
        {
            _moduleLookupState->addressFailure = std::move(message);
            _moduleLookupState->addressStatus  = status;
        }

        void setMemoryBytes(std::vector<std::byte> bytes)
        {
            _memoryState->bytes = std::move(bytes);
        }

        void failMemoryWith(std::string message)
        {
            _memoryState->failure = std::move(message);
        }

        void failWritesWith(std::string message)
        {
            _memoryState->writeFailure = std::move(message);
        }

        void setShortRead(bool shortRead)
        {
            _memoryState->shortRead = shortRead;
        }

        [[nodiscard]] std::shared_ptr<FakeMemoryState> memoryState() const
        {
            return _memoryState;
        }

        [[nodiscard]] std::shared_ptr<FakeScatterState> scatterState() const
        {
            return _scatterState;
        }

        void setModules(std::vector<ModuleDescriptor> modules)
        {
            _moduleState->modules = std::move(modules);
        }

        void failModulesWith(std::string message)
        {
            _moduleState->failure = std::move(message);
        }

        void setRegions(std::vector<MemoryMapRegion> regions)
        {
            _mapState->regions = std::move(regions);
        }

        void failMapsWith(std::string message)
        {
            _mapState->failure = std::move(message);
        }

        [[nodiscard]] std::shared_ptr<FakeModuleState> moduleState() const
        {
            return _moduleState;
        }

        [[nodiscard]] std::shared_ptr<FakeMapState> mapState() const
        {
            return _mapState;
        }

        void setVadExEntries(std::vector<VadExDescriptor> entries)
        {
            _vadExState->entries = std::move(entries);
        }

        void failVadExWith(std::string message, LookupStatus status = LookupStatus::Failed)
        {
            _vadExState->failure = std::move(message);
            _vadExState->status  = status;
        }

        [[nodiscard]] std::shared_ptr<FakeVadExState> vadExState() const
        {
            return _vadExState;
        }

        void setThreads(std::vector<ThreadDescriptor> threads)
        {
            _threadState->threads = std::move(threads);
        }

        void failThreadsWith(std::string message)
        {
            _threadState->failure = std::move(message);
        }

        [[nodiscard]] std::shared_ptr<FakeThreadState> threadState() const
        {
            return _threadState;
        }

        void setVms(std::vector<VmDescriptor> vms)
        {
            _vmState->vms = std::move(vms);
        }

        void failVmsWith(std::string message)
        {
            _vmState->failure = std::move(message);
        }

        [[nodiscard]] std::shared_ptr<FakeVmState> vmState() const
        {
            return _vmState;
        }

        [[nodiscard]] std::shared_ptr<FakePhysicalState> physicalState() const
        {
            return _physicalState;
        }

        void setVfsEntries(std::vector<VfsEntry> entries)
        {
            _vfsState->entries = std::move(entries);
        }

        void setVfsFile(std::string path, std::vector<std::byte> bytes)
        {
            _vfsState->files.insert_or_assign(std::move(path), std::move(bytes));
        }

        void failVfsWith(std::string message)
        {
            _vfsState->failure     = message;
            _vfsState->readFailure = std::move(message);
        }

        void failVfsWriteWith(std::string message)
        {
            _vfsState->writeFailure = std::move(message);
        }

        [[nodiscard]] std::shared_ptr<FakeVfsState> vfsState() const
        {
            return _vfsState;
        }

        void setVirtualRegion(VirtualRegion region)
        {
            _virtualQueryState->region = std::move(region);
        }

        void failVirtualQueryWith(std::string message, LookupStatus status = LookupStatus::Failed)
        {
            _virtualQueryState->failure = std::move(message);
            _virtualQueryState->status  = status;
        }

        [[nodiscard]] std::shared_ptr<FakeVirtualQueryState> virtualQueryState() const
        {
            return _virtualQueryState;
        }

        void setPageItems(std::vector<PageReadItem> pages)
        {
            _pageState->pages = std::move(pages);
        }

        void failPagesWith(std::string message)
        {
            _pageState->failure = std::move(message);
        }

        void failPrefetchWith(std::string message)
        {
            _pageState->prefetchFailure = std::move(message);
        }

        [[nodiscard]] std::shared_ptr<FakePageState> pageState() const
        {
            return _pageState;
        }

        void setSearchHits(std::vector<std::uint64_t> hits, bool truncated = false)
        {
            _searchState->hits      = std::move(hits);
            _searchState->truncated = truncated;
        }

        void failSearchWith(std::string message)
        {
            _searchState->failure = std::move(message);
        }

        [[nodiscard]] std::shared_ptr<FakeSearchState> searchState() const
        {
            return _searchState;
        }

        [[nodiscard]] std::shared_ptr<FakeHandleListState> handleListState() const
        {
            return _handleListState;
        }

        [[nodiscard]] std::shared_ptr<FakeModuleTableState> moduleTableState() const
        {
            return _moduleTableState;
        }

        [[nodiscard]] std::shared_ptr<FakeUnloadedModuleState> unloadedModuleState() const
        {
            return _unloadedModuleState;
        }

        [[nodiscard]] std::shared_ptr<FakeServiceState> serviceState() const
        {
            return _serviceState;
        }

        [[nodiscard]] std::shared_ptr<FakeUserState> userState() const
        {
            return _userState;
        }

        [[nodiscard]] std::shared_ptr<FakeNetworkState> networkState() const
        {
            return _networkState;
        }

        [[nodiscard]] std::shared_ptr<FakeHeapState> heapState() const
        {
            return _heapState;
        }

        [[nodiscard]] std::shared_ptr<FakeHeapAllocState> heapAllocState() const
        {
            return _heapAllocState;
        }

        [[nodiscard]] std::shared_ptr<FakeKernelDriverState> kernelDriverState() const
        {
            return _kernelDriverState;
        }

        [[nodiscard]] std::shared_ptr<FakeKernelDeviceState> kernelDeviceState() const
        {
            return _kernelDeviceState;
        }

        [[nodiscard]] std::shared_ptr<FakeKernelObjectState> kernelObjectState() const
        {
            return _kernelObjectState;
        }

        void setPteEntries(std::vector<PteDescriptor> entries)
        {
            _pteState->entries = std::move(entries);
        }

        void failPteWith(std::string message, LookupStatus status = LookupStatus::Failed)
        {
            _pteState->failure = std::move(message);
            _pteState->status  = status;
        }

        [[nodiscard]] std::shared_ptr<FakePteState> pteState() const
        {
            return _pteState;
        }

        void setPoolEntries(std::vector<PoolDescriptor> entries)
        {
            _poolState->entries = std::move(entries);
        }

        void setPoolTags(std::vector<PoolTagDescriptor> tags)
        {
            _poolState->tags = std::move(tags);
        }

        void failPoolWith(std::string message, LookupStatus status = LookupStatus::Failed)
        {
            _poolState->failure = std::move(message);
            _poolState->status  = status;
        }

        [[nodiscard]] std::shared_ptr<FakePoolState> poolState() const
        {
            return _poolState;
        }

        void setPfnEntries(std::vector<PfnDescriptor> entries)
        {
            _pfnState->entries = std::move(entries);
        }

        void failPfnWith(std::string message, LookupStatus status = LookupStatus::Failed)
        {
            _pfnState->failure = std::move(message);
            _pfnState->status  = status;
        }

        [[nodiscard]] std::shared_ptr<FakePfnState> pfnState() const
        {
            return _pfnState;
        }

        void failConfigWith(std::string message)
        {
            _configState->failure = std::move(message);
        }

        [[nodiscard]] std::shared_ptr<FakeConfigState> configState() const
        {
            return _configState;
        }

        void setHandlesBare(bool bare)
        {
            std::lock_guard lock {_mutex};
            _bareHandles = bare;
        }

        [[nodiscard]] std::optional<ProcessFilter> lastProcessFilter() const
        {
            return _processState->lastFilter;
        }

        [[nodiscard]] std::optional<ProcessFilter> lastAllProcessFilter() const
        {
            return _processState->lastAllFilter;
        }

        [[nodiscard]] std::shared_ptr<VmHandle> acquire(const VmTarget& target, std::string& error) override
        {
            std::lock_guard lock {_mutex};
            if (!_failMessage.empty())
            {
                error = _failMessage;
                return nullptr;
            }
            ++_acquires;
            std::shared_ptr<VmHandle> handle;
            if (_bareHandles)
            {
                handle = std::make_shared<BareHandle>();
            }
            else
            {
                handle = std::make_shared<FakeHandle>(target.name,
                                                      _processState,
                                                      _processInfoState,
                                                      _moduleLookupState,
                                                      _pdbState,
                                                      _memoryState,
                                                      _scatterState,
                                                      _moduleState,
                                                      _mapState,
                                                      _vadExState,
                                                      _threadState,
                                                      _vmState,
                                                      _physicalState,
                                                      _vfsState,
                                                      _virtualQueryState,
                                                      _pageState,
                                                      _searchState,
                                                      _handleListState,
                                                      _moduleTableState,
                                                      _unloadedModuleState,
                                                      _serviceState,
                                                      _userState,
                                                      _networkState,
                                                      _heapState,
                                                      _heapAllocState,
                                                      _kernelDriverState,
                                                      _kernelDeviceState,
                                                      _kernelObjectState,
                                                      _pteState,
                                                      _poolState,
                                                      _pfnState,
                                                      _configState);
            }
            _handles.push_back(handle);
            return handle;
        }

        [[nodiscard]] int acquireCount() const
        {
            std::lock_guard lock {_mutex};
            return _acquires;
        }

        [[nodiscard]] int liveHandleCount() const
        {
            std::lock_guard lock {_mutex};
            int             count = 0;
            for (const auto& handle : _handles)
            {
                if (!handle.expired())
                {
                    ++count;
                }
            }
            return count;
        }

    private:
        mutable std::mutex                       _mutex;
        std::vector<std::weak_ptr<VmHandle>>     _handles;
        int                                      _acquires {};
        bool                                     _bareHandles {};
        std::string                              _failMessage;
        std::shared_ptr<FakeProcessState>        _processState {std::make_shared<FakeProcessState>()};
        std::shared_ptr<FakeProcessInfoState>    _processInfoState {std::make_shared<FakeProcessInfoState>()};
        std::shared_ptr<FakeModuleLookupState>   _moduleLookupState {std::make_shared<FakeModuleLookupState>()};
        std::shared_ptr<FakePdbState>            _pdbState {std::make_shared<FakePdbState>()};
        std::shared_ptr<FakeMemoryState>         _memoryState {std::make_shared<FakeMemoryState>()};
        std::shared_ptr<FakeScatterState>        _scatterState {std::make_shared<FakeScatterState>()};
        std::shared_ptr<FakeModuleState>         _moduleState {std::make_shared<FakeModuleState>()};
        std::shared_ptr<FakeMapState>            _mapState {std::make_shared<FakeMapState>()};
        std::shared_ptr<FakeVadExState>          _vadExState {std::make_shared<FakeVadExState>()};
        std::shared_ptr<FakeThreadState>         _threadState {std::make_shared<FakeThreadState>()};
        std::shared_ptr<FakeVmState>             _vmState {std::make_shared<FakeVmState>()};
        std::shared_ptr<FakePhysicalState>       _physicalState {std::make_shared<FakePhysicalState>()};
        std::shared_ptr<FakeVfsState>            _vfsState {std::make_shared<FakeVfsState>()};
        std::shared_ptr<FakeVirtualQueryState>   _virtualQueryState {std::make_shared<FakeVirtualQueryState>()};
        std::shared_ptr<FakePageState>           _pageState {std::make_shared<FakePageState>()};
        std::shared_ptr<FakeSearchState>         _searchState {std::make_shared<FakeSearchState>()};
        std::shared_ptr<FakeHandleListState>     _handleListState {std::make_shared<FakeHandleListState>()};
        std::shared_ptr<FakeModuleTableState>    _moduleTableState {std::make_shared<FakeModuleTableState>()};
        std::shared_ptr<FakeUnloadedModuleState> _unloadedModuleState {std::make_shared<FakeUnloadedModuleState>()};
        std::shared_ptr<FakeServiceState>        _serviceState {std::make_shared<FakeServiceState>()};
        std::shared_ptr<FakeUserState>           _userState {std::make_shared<FakeUserState>()};
        std::shared_ptr<FakeNetworkState>        _networkState {std::make_shared<FakeNetworkState>()};
        std::shared_ptr<FakeHeapState>           _heapState {std::make_shared<FakeHeapState>()};
        std::shared_ptr<FakeHeapAllocState>      _heapAllocState {std::make_shared<FakeHeapAllocState>()};
        std::shared_ptr<FakeKernelDriverState>   _kernelDriverState {std::make_shared<FakeKernelDriverState>()};
        std::shared_ptr<FakeKernelDeviceState>   _kernelDeviceState {std::make_shared<FakeKernelDeviceState>()};
        std::shared_ptr<FakeKernelObjectState>   _kernelObjectState {std::make_shared<FakeKernelObjectState>()};
        std::shared_ptr<FakePteState>            _pteState {std::make_shared<FakePteState>()};
        std::shared_ptr<FakePoolState>           _poolState {std::make_shared<FakePoolState>()};
        std::shared_ptr<FakePfnState>            _pfnState {std::make_shared<FakePfnState>()};
        std::shared_ptr<FakeConfigState>         _configState {std::make_shared<FakeConfigState>()};
    };

    // Minimal inner backend for the handle cache: counts acquisitions, can hold them inside acquire() and can be
    // switched to fail, so reuse, single flight and eviction are observable without a real MemProcFS handle.
    class CountingBackend : public VmBackend
    {
    public:
        [[nodiscard]] std::shared_ptr<VmHandle> acquire(const VmTarget& target, std::string& error) override
        {
            std::unique_lock lock {_mutex};
            ++_acquires;
            _lastTarget = target;
            _enteredCv.notify_all();
            _gateCv.wait(lock,
                         [this]
                         {
                             return !_blocked;
                         });
            const std::string failure = _failMessage;
            lock.unlock();

            if (!failure.empty())
            {
                error = failure;
                return nullptr;
            }

            auto handle = std::make_shared<BareHandle>();
            lock.lock();
            _handles.push_back(handle);
            return handle;
        }

        void blockAcquires()
        {
            std::lock_guard lock {_mutex};
            _blocked = true;
        }

        void releaseAcquires()
        {
            {
                std::lock_guard lock {_mutex};
                _blocked = false;
            }
            _gateCv.notify_all();
        }

        void failWith(std::string message)
        {
            std::lock_guard lock {_mutex};
            _failMessage = std::move(message);
        }

        void clearFailure()
        {
            std::lock_guard lock {_mutex};
            _failMessage.clear();
        }

        [[nodiscard]] bool waitForAcquires(int expected, std::chrono::milliseconds timeout)
        {
            std::unique_lock lock {_mutex};
            return _enteredCv.wait_for(lock,
                                       timeout,
                                       [this, expected]
                                       {
                                           return _acquires >= expected;
                                       });
        }

        [[nodiscard]] int acquireCount() const
        {
            std::lock_guard lock {_mutex};
            return _acquires;
        }

        [[nodiscard]] int liveHandleCount() const
        {
            std::lock_guard lock {_mutex};
            int             count = 0;
            for (const auto& handle : _handles)
            {
                if (!handle.expired())
                {
                    ++count;
                }
            }
            return count;
        }

        [[nodiscard]] VmTarget lastTarget() const
        {
            std::lock_guard lock {_mutex};
            return _lastTarget;
        }

    private:
        mutable std::mutex                   _mutex;
        std::condition_variable              _enteredCv;
        std::condition_variable              _gateCv;
        std::vector<std::weak_ptr<VmHandle>> _handles;
        int                                  _acquires {};
        bool                                 _blocked {};
        std::string                          _failMessage;
        VmTarget                             _lastTarget;
    };
} // namespace mpfshost::testing

#endif // MPFSHOST_TESTS_SUPPORT_FAKE_BACKEND_HPP
