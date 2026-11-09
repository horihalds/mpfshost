#ifndef MPFSHOST_CORE_CLIENT_CLIENT_LEGACY_HPP
#define MPFSHOST_CORE_CLIENT_CLIENT_LEGACY_HPP
#pragma once

#include "core/ipc/message.hpp"
#include "mpfshost/client.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace mpfshost
{
    struct ProcessQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        std::string   nameFilter;
    };

    [[nodiscard]] std::optional<ListTargetsResponse>  listTargets(const std::filesystem::path& socketPath,
                                                                  std::string&                 error);
    [[nodiscard]] std::optional<CapabilitiesResponse> getCapabilities(const std::filesystem::path& socketPath,
                                                                      std::string&                 error);
    [[nodiscard]] std::optional<BindResponse>
    bindTarget(const std::filesystem::path& socketPath, const std::string& targetName, std::string& error);
    [[nodiscard]] std::optional<ProcessListResponse>
    listProcesses(const std::filesystem::path& socketPath, const ProcessQuery& query, std::string& error);

    [[nodiscard]] std::optional<ProcessListAllResponse>
    listAllProcesses(const std::filesystem::path& socketPath, const ProcessQuery& query, std::string& error);

    struct ProcessInfoQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        std::string   name; // resolves the pid when it is 0
    };

    [[nodiscard]] std::optional<ProcessInfoResponse>
    processInfo(const std::filesystem::path& socketPath, const ProcessInfoQuery& query, std::string& error);

    struct ProcessByDtbQuery
    {
        std::string   targetName;
        std::uint64_t dtb {};
    };

    [[nodiscard]] std::optional<ProcessByDtbResponse>
    processByDtb(const std::filesystem::path& socketPath, const ProcessByDtbQuery& query, std::string& error);

    struct DebugQuery
    {
        std::string targetName;
    };

    // Reports QEMU's run state and the host's own debug bookkeeping without changing anything.
    [[nodiscard]] std::optional<DebugStatus>
    debugState(const std::filesystem::path& socketPath, const DebugQuery& query, std::string& error);

    // Stops or continues the guest and answers the resulting debug status.
    [[nodiscard]] std::optional<DebugStatus> debugControl(const std::filesystem::path& socketPath,
                                                          const DebugQuery&            query,
                                                          DebugControlAction           action,
                                                          std::string&                 error);

    struct DebugRegistersQuery
    {
        std::string   targetName;
        std::uint32_t vcpu {kDebugAllVcpus}; // kDebugAllVcpus selects every vCPU
    };

    [[nodiscard]] std::optional<std::vector<DebugRegisterRecord>>
    debugRegisters(const std::filesystem::path& socketPath, const DebugRegistersQuery& query, std::string& error);

    struct DebugThreadsQuery
    {
        std::string targetName;
    };

    [[nodiscard]] std::optional<std::vector<DebugThreadRecord>>
    debugThreads(const std::filesystem::path& socketPath, const DebugThreadsQuery& query, std::string& error);

    struct DisassembleQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        std::uint64_t address {};
        std::uint32_t count {16};
    };

    // Reads a bounded window of process memory and decodes it. Needs no debugger attachment.
    [[nodiscard]] std::optional<DisassembleResponse>
    disassemble(const std::filesystem::path& socketPath, const DisassembleQuery& query, std::string& error);

    struct DebugBacktraceQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        std::uint32_t vcpu {0};
        std::string   threadId; // when set, selects this GDB thread instead of the vCPU index
        std::uint32_t maxFrames {32};
    };

    [[nodiscard]] std::optional<DebugBacktraceResponse>
    debugBacktrace(const std::filesystem::path& socketPath, const DebugBacktraceQuery& query, std::string& error);

    struct DebugWatchQuery
    {
        std::string      targetName;
        std::uint32_t    pid {};
        std::uint64_t    address {}; // offset from the module or symbol when `module` is set
        std::uint32_t    width {4};
        DebugWatchAccess access {DebugWatchAccess::Write};
        std::uint32_t    maxHits {1};
        std::uint32_t    timeoutMs {30000};
        std::string      module; // optional symbolic anchor
        std::string      symbol; // export or PDB name, empty for a module-relative offset
    };

    // Streams one hit at a time through `onHit` and returns the end record. A dedicated connection is used
    // for each watch, so cancellation means closing that connection.
    [[nodiscard]] std::optional<DebugWatchEnd>
    debugWatch(const std::filesystem::path&                           socketPath,
               const DebugWatchQuery&                                 query,
               const std::function<void(const DebugWatchData&)>&      onHit,
               const std::function<void(const DebugWatchHeartbeat&)>& onHeartbeat,
               std::string&                                           error);

    [[nodiscard]] std::optional<DebugWatchEnd> debugWatch(const std::filesystem::path&                      socketPath,
                                                          const DebugWatchQuery&                            query,
                                                          const std::function<void(const DebugWatchData&)>& onHit,
                                                          std::string&                                      error);

    struct DebugBreakpointQuery
    {
        std::string         targetName;
        std::uint32_t       pid {};
        std::uint64_t       address {}; // offset from the module or symbol when `module` is set
        DebugBreakpointKind kind {DebugBreakpointKind::Software};
        std::uint32_t       timeoutMs {30000};
        std::string         module; // optional symbolic anchor
        std::string         symbol; // export or PDB name, empty for a module-relative offset
    };

    // Arms one breakpoint, runs to its first hit or a timeout, and always tears the breakpoint down.
    [[nodiscard]] std::optional<DebugBreakpointResponse>
    debugBreakpoint(const std::filesystem::path& socketPath, const DebugBreakpointQuery& query, std::string& error);

    struct DebugStepQuery
    {
        DebugStepQuery() = default;

        DebugStepQuery(std::string target, std::uint32_t selectedVcpu, std::string selectedThread = {})
            : targetName {std::move(target)}, vcpu {selectedVcpu}, threadId {std::move(selectedThread)}
        {
        }

        std::string   targetName;
        std::uint32_t vcpu {0};
        std::string   threadId; // when set, selects this GDB thread instead of the vCPU index
    };

    // Executes one instruction on a paused guest and returns the resulting registers.
    [[nodiscard]] std::optional<DebugStepResponse>
    debugStep(const std::filesystem::path& socketPath, const DebugStepQuery& query, std::string& error);

    struct DebugSetRegisterQuery
    {
        std::string   targetName;
        std::uint32_t vcpu {0};
        std::string   threadId; // when set, selects this GDB thread instead of the vCPU index
        std::string   name;
        std::uint64_t value {};
    };

    // Writes one register of a stopped thread and returns the thread's record afterwards.
    [[nodiscard]] std::optional<DebugSetRegisterResponse>
    debugSetRegister(const std::filesystem::path& socketPath, const DebugSetRegisterQuery& query, std::string& error);

    // Starts one persistent GDB stub owned by this connection and pauses the guest if needed. The returned
    // attachment lasts until this function returns (the connection closes) or another client runs debug-detach.
    // `whileAttached` runs after the attach response and before the connection is closed.
    [[nodiscard]] std::optional<DebugAttachResponse>
    debugAttach(const std::filesystem::path&                           socketPath,
                const DebugQuery&                                      query,
                const std::function<void(const DebugAttachResponse&)>& whileAttached,
                std::string&                                           error);

    // Stops an active GDB stub, including one this host did not start, and reports the resulting status.
    [[nodiscard]] std::optional<DebugDetachResponse>
    debugDetach(const std::filesystem::path& socketPath, const DebugQuery& query, std::string& error);

    struct DebugBreakpointArmQuery
    {
        std::string         targetName;
        std::uint32_t       pid {};
        std::uint64_t       address {};
        DebugBreakpointKind kind {DebugBreakpointKind::Software};
    };

    [[nodiscard]] std::optional<DebugBreakpointArmResponse> debugArmBreakpoint(const std::filesystem::path& socketPath,
                                                                               const DebugBreakpointArmQuery& query,
                                                                               std::string&                   error);

    struct DebugResourceRemoveQuery
    {
        std::string   targetName;
        std::uint32_t id {};
    };

    [[nodiscard]] std::optional<DebugResourceRemoveResponse> debugRemoveBreakpoint(
        const std::filesystem::path& socketPath, const DebugResourceRemoveQuery& query, std::string& error);

    [[nodiscard]] std::optional<std::vector<DebugBreakpointArmResponse>>
    debugListBreakpoints(const std::filesystem::path& socketPath, const DebugQuery& query, std::string& error);

    struct DebugWatchArmQuery
    {
        std::string      targetName;
        std::uint32_t    pid {};
        std::uint64_t    address {};
        std::uint32_t    width {4};
        DebugWatchAccess access {DebugWatchAccess::Write};
    };

    [[nodiscard]] std::optional<DebugWatchArmResponse>
    debugArmWatch(const std::filesystem::path& socketPath, const DebugWatchArmQuery& query, std::string& error);

    [[nodiscard]] std::optional<DebugResourceRemoveResponse> debugRemoveWatch(const std::filesystem::path& socketPath,
                                                                              const DebugResourceRemoveQuery& query,
                                                                              std::string&                    error);

    [[nodiscard]] std::optional<std::vector<DebugWatchArmResponse>>
    debugListWatches(const std::filesystem::path& socketPath, const DebugQuery& query, std::string& error);

    // Resumes the attached stub without removing any armed breakpoint or watchpoint.
    [[nodiscard]] std::optional<DebugStatus>
    debugContinueArmed(const std::filesystem::path& socketPath, const DebugQuery& query, std::string& error);

    struct DebugWaitQuery
    {
        std::string   targetName;
        std::uint32_t timeoutMs {30000};
        std::uint32_t maxHits {1};
    };

    // Streams one stop per armed resource through `onHit` and returns the end record.
    [[nodiscard]] std::optional<DebugWaitEnd> debugWait(const std::filesystem::path&                    socketPath,
                                                        const DebugWaitQuery&                           query,
                                                        const std::function<void(const DebugWaitHit&)>& onHit,
                                                        std::string&                                    error);

    struct VirtualQueryQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        std::uint64_t address {};
    };

    [[nodiscard]] std::optional<VirtualRegion>
    queryVirtual(const std::filesystem::path& socketPath, const VirtualQueryQuery& query, std::string& error);

    struct PageReadQuery
    {
        std::string            targetName;
        std::uint32_t          pid {};
        std::vector<PageRange> ranges;
    };

    [[nodiscard]] std::optional<std::vector<PageReadItem>>
    readPages(const std::filesystem::path& socketPath, const PageReadQuery& query, std::string& error);

    struct PrefetchQuery
    {
        std::string            targetName;
        std::uint32_t          pid {};
        std::vector<PageRange> ranges;
    };

    [[nodiscard]] std::optional<std::uint32_t>
    prefetchPages(const std::filesystem::path& socketPath, const PrefetchQuery& query, std::string& error);

    struct ConfigSetQuery
    {
        std::string   targetName;
        ConfigOption  option {};
        std::uint32_t pid {};
        std::uint64_t value {};
    };

    [[nodiscard]] std::optional<std::uint64_t>
    setConfig(const std::filesystem::path& socketPath, const ConfigSetQuery& query, std::string& error);

    struct MemorySearchQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        SearchTerm    term;
        std::uint32_t alignBytes {1};
        std::uint64_t minAddress {};
        std::uint64_t maxAddress {};
        std::uint32_t maxResults {};
    };

    [[nodiscard]] std::optional<SearchResult>
    searchMemory(const std::filesystem::path& socketPath, const MemorySearchQuery& query, std::string& error);

    struct ModuleQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        std::string   nameFilter;
        std::uint16_t flags {};
    };

    [[nodiscard]] std::optional<std::vector<ModuleDescriptor>>
    listModules(const std::filesystem::path& socketPath, const ModuleQuery& query, std::string& error);

    struct SymbolQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        std::string   module;
        std::string   symbol; // empty resolves the module base only
    };

    [[nodiscard]] std::optional<SymbolLookupResponse>
    lookupSymbol(const std::filesystem::path& socketPath, const SymbolQuery& query, std::string& error);

    struct PdbLoadQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        std::uint64_t moduleBase {};
    };

    [[nodiscard]] std::optional<PdbLoadResponse>
    pdbLoad(const std::filesystem::path& socketPath, const PdbLoadQuery& query, std::string& error);

    struct PdbSymbolNameQuery
    {
        std::string   targetName;
        std::string   module;
        std::uint64_t address {};
    };

    [[nodiscard]] std::optional<PdbSymbolNameResponse>
    pdbSymbolName(const std::filesystem::path& socketPath, const PdbSymbolNameQuery& query, std::string& error);

    struct PdbSymbolAddressQuery
    {
        std::string targetName;
        std::string module;
        std::string symbol;
    };

    [[nodiscard]] std::optional<PdbSymbolAddressResponse>
    pdbSymbolAddress(const std::filesystem::path& socketPath, const PdbSymbolAddressQuery& query, std::string& error);

    struct PdbTypeSizeQuery
    {
        std::string targetName;
        std::string module;
        std::string type;
    };

    [[nodiscard]] std::optional<PdbTypeSizeResponse>
    pdbTypeSize(const std::filesystem::path& socketPath, const PdbTypeSizeQuery& query, std::string& error);

    struct PdbTypeChildOffsetQuery
    {
        std::string targetName;
        std::string module;
        std::string type;
        std::string child;
    };

    [[nodiscard]] std::optional<PdbTypeChildOffsetResponse> pdbTypeChildOffset(const std::filesystem::path& socketPath,
                                                                               const PdbTypeChildOffsetQuery& query,
                                                                               std::string&                   error);

    struct ModuleLayoutQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        std::string   module;
    };

    [[nodiscard]] std::optional<ModuleLayoutResponse>
    moduleLayout(const std::filesystem::path& socketPath, const ModuleLayoutQuery& query, std::string& error);

    struct ModuleByAddressQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        std::uint64_t address {};
    };

    [[nodiscard]] std::optional<ModuleByAddressResponse>
    moduleByAddress(const std::filesystem::path& socketPath, const ModuleByAddressQuery& query, std::string& error);

    struct AddressResolutionQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        std::uint64_t address {};
    };

    [[nodiscard]] std::optional<AddressResolution>
    resolveAddress(const std::filesystem::path& socketPath, const AddressResolutionQuery& query, std::string& error);

    struct MemoryMapQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        bool          identifyModules {};
    };

    [[nodiscard]] std::optional<std::vector<MemoryMapRegion>>
    listMemoryMap(const std::filesystem::path& socketPath, const MemoryMapQuery& query, std::string& error);

    struct VadExQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        std::uint32_t pageOffset {};
        std::uint32_t pageCount {};
    };

    [[nodiscard]] std::optional<std::vector<VadExDescriptor>>
    listVadEx(const std::filesystem::path& socketPath, const VadExQuery& query, std::string& error);

    struct ThreadQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        std::uint32_t tid {};
        std::uint16_t flags {};
    };

    [[nodiscard]] std::optional<std::vector<ThreadDescriptor>>
    listThreads(const std::filesystem::path& socketPath, const ThreadQuery& query, std::string& error);

    struct HandleQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        std::string   filter;
    };

    [[nodiscard]] std::optional<std::vector<HandleDescriptor>>
    listHandles(const std::filesystem::path& socketPath, const HandleQuery& query, std::string& error);

    struct ModuleTableQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        std::string   module;
    };

    [[nodiscard]] std::optional<std::vector<ExportDescriptor>>
    listExports(const std::filesystem::path& socketPath, const ModuleTableQuery& query, std::string& error);

    [[nodiscard]] std::optional<std::vector<ImportDescriptor>>
    listImports(const std::filesystem::path& socketPath, const ModuleTableQuery& query, std::string& error);

    struct UnloadedModuleQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        std::string   filter;
    };

    [[nodiscard]] std::optional<std::vector<UnloadedModuleDescriptor>>
    listUnloadedModules(const std::filesystem::path& socketPath, const UnloadedModuleQuery& query, std::string& error);

    struct ServiceQuery
    {
        std::string targetName;
        std::string filter;
    };

    struct ServiceListAnswer
    {
        std::vector<ServiceDescriptor> services;
        bool                           truncated {};
    };

    [[nodiscard]] std::optional<ServiceListAnswer>
    listServices(const std::filesystem::path& socketPath, const ServiceQuery& query, std::string& error);

    struct UserQuery
    {
        std::string targetName;
        std::string filter;
    };

    struct UserListAnswer
    {
        std::vector<UserDescriptor> users;
        bool                        truncated {};
    };

    [[nodiscard]] std::optional<UserListAnswer>
    listUsers(const std::filesystem::path& socketPath, const UserQuery& query, std::string& error);

    struct NetworkQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        std::string   filter;
    };

    struct NetworkListAnswer
    {
        std::vector<NetworkEntryDescriptor> entries;
        bool                                truncated {};
    };

    [[nodiscard]] std::optional<NetworkListAnswer>
    listNetwork(const std::filesystem::path& socketPath, const NetworkQuery& query, std::string& error);

    struct HeapQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
    };

    struct HeapListAnswer
    {
        std::vector<HeapDescriptor> heaps;
        bool                        truncated {};
    };

    [[nodiscard]] std::optional<HeapListAnswer>
    listHeaps(const std::filesystem::path& socketPath, const HeapQuery& query, std::string& error);

    struct HeapAllocQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        std::uint64_t heap {};
    };

    struct HeapAllocListAnswer
    {
        std::vector<HeapAllocDescriptor> allocations;
        bool                             truncated {};
    };

    [[nodiscard]] std::optional<HeapAllocListAnswer>
    listHeapAllocations(const std::filesystem::path& socketPath, const HeapAllocQuery& query, std::string& error);

    struct KernelDriverQuery
    {
        std::string targetName;
        std::string filter;
    };

    struct KernelDriverAnswer
    {
        std::vector<KernelDriverDescriptor> drivers;
        bool                                truncated {};
    };

    [[nodiscard]] std::optional<KernelDriverAnswer>
    listKernelDrivers(const std::filesystem::path& socketPath, const KernelDriverQuery& query, std::string& error);

    struct KernelDeviceQuery
    {
        std::string targetName;
        std::string filter;
    };

    struct KernelDeviceAnswer
    {
        std::vector<KernelDeviceDescriptor> devices;
        bool                                truncated {};
    };

    [[nodiscard]] std::optional<KernelDeviceAnswer>
    listKernelDevices(const std::filesystem::path& socketPath, const KernelDeviceQuery& query, std::string& error);

    struct KernelObjectQuery
    {
        std::string targetName;
        std::string filter;
    };

    struct KernelObjectAnswer
    {
        std::vector<KernelObjectDescriptor> objects;
        bool                                truncated {};
    };

    [[nodiscard]] std::optional<KernelObjectAnswer>
    listKernelObjects(const std::filesystem::path& socketPath, const KernelObjectQuery& query, std::string& error);

    struct PteQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        bool          identifyModules {};
        std::string   filter;
        std::uint64_t minAddress {};
        std::uint64_t maxAddress {};
    };

    struct PteAnswer
    {
        std::vector<PteDescriptor> regions;
        std::uint64_t              mapEntries {};
        bool                       truncated {};
    };

    [[nodiscard]] std::optional<PteAnswer>
    listPte(const std::filesystem::path& socketPath, const PteQuery& query, std::string& error);

    struct PoolQuery
    {
        std::string   targetName;
        bool          big {};
        std::string   tagFilter;
        std::uint32_t minBytes {};
        std::uint64_t minAddress {};
        std::uint64_t maxAddress {};
    };

    struct PoolAnswer
    {
        std::vector<PoolDescriptor> entries;
        std::uint64_t               mapEntries {};
        std::uint64_t               mapBytes {};
        bool                        truncated {};
    };

    [[nodiscard]] std::optional<PoolAnswer>
    listPool(const std::filesystem::path& socketPath, const PoolQuery& query, std::string& error);

    struct PoolTagQuery
    {
        std::string targetName;
        bool        big {};
        std::string tagFilter;
    };

    struct PoolTagAnswer
    {
        std::vector<PoolTagDescriptor> tags;
        std::uint64_t                  mapTags {};
        std::uint64_t                  mapEntries {};
        bool                           truncated {};
    };

    [[nodiscard]] std::optional<PoolTagAnswer>
    listPoolTags(const std::filesystem::path& socketPath, const PoolTagQuery& query, std::string& error);

    struct PfnQuery
    {
        std::string   targetName;
        std::uint32_t pfnStart {};
        std::uint32_t pfnCount {};
    };

    struct PfnAnswer
    {
        std::vector<PfnDescriptor> entries;
    };

    [[nodiscard]] std::optional<PfnAnswer>
    listPfn(const std::filesystem::path& socketPath, const PfnQuery& query, std::string& error);

    struct VfsQuery
    {
        std::string targetName;
        std::string path;
        std::string nameFilter;
    };

    [[nodiscard]] std::optional<std::vector<VfsEntry>>
    listVfs(const std::filesystem::path& socketPath, const VfsQuery& query, std::string& error);

    struct VfsReadQuery
    {
        std::string   targetName;
        std::string   path;
        std::uint64_t offset {};
        std::uint32_t size {};
    };

    [[nodiscard]] std::optional<std::vector<std::byte>>
    readVfs(const std::filesystem::path& socketPath, const VfsReadQuery& query, std::string& error);

    struct VfsWriteCommand
    {
        std::string            targetName;
        std::string            path;
        std::uint64_t          offset {};
        std::vector<std::byte> data;
    };

    [[nodiscard]] std::optional<std::uint64_t>
    writeVfs(const std::filesystem::path& socketPath, const VfsWriteCommand& command, std::string& error);

    struct MemoryQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        std::uint64_t address {};
        std::uint32_t size {};
    };

    struct MemoryWriteCommand
    {
        std::string            targetName;
        std::uint32_t          pid {};
        std::uint64_t          address {};
        std::vector<std::byte> data;
    };

    struct ScatterReadQuery
    {
        std::string               targetName;
        std::uint32_t             pid {};
        std::vector<ScatterRange> ranges;
    };

    class ScatterSession
    {
    public:
        ScatterSession(int fd, std::uint32_t scatterId, bool ownsFd = true) noexcept;
        ~ScatterSession();

        ScatterSession(const ScatterSession&)            = delete;
        ScatterSession& operator=(const ScatterSession&) = delete;
        ScatterSession(ScatterSession&& other) noexcept;
        ScatterSession& operator=(ScatterSession&& other) noexcept;

        [[nodiscard]] std::uint32_t scatterId() const noexcept;

        [[nodiscard]] std::optional<std::uint32_t> prepare(const std::vector<ScatterItem>& entries, std::string& error);
        [[nodiscard]] std::optional<std::vector<ScatterReadResult>> execute(std::string& error);
        [[nodiscard]] std::optional<std::vector<ScatterReadResult>> executeRead(std::string& error);
        [[nodiscard]] bool                                          clear(std::string& error);
        [[nodiscard]] bool                                          close(std::string& error);

    private:
        int           _fd {-1};
        std::uint32_t _scatterId {};
        bool          _ownsFd {true};
    };

    [[nodiscard]] std::optional<ScatterSession> createScatter(const std::filesystem::path& socketPath,
                                                              const std::string&           targetName,
                                                              std::uint32_t                pid,
                                                              std::string&                 error);

    [[nodiscard]] std::optional<std::vector<std::byte>>
    readMemory(const std::filesystem::path& socketPath, const MemoryQuery& query, std::string& error);

    // Returns the readable prefix, which may be empty when the first page is unreadable.
    [[nodiscard]] std::optional<std::vector<std::byte>>
    readMemoryPartial(const std::filesystem::path& socketPath, const MemoryQuery& query, std::string& error);
    [[nodiscard]] std::optional<std::size_t> readMemoryInto(const std::filesystem::path& socketPath,
                                                            const MemoryQuery&           query,
                                                            std::span<std::byte>         out,
                                                            std::string&                 error);

    [[nodiscard]] std::optional<std::uint64_t>
    writeMemory(const std::filesystem::path& socketPath, const MemoryWriteCommand& command, std::string& error);

    [[nodiscard]] std::optional<std::vector<ScatterReadResult>>
    scatterRead(const std::filesystem::path& socketPath, const ScatterReadQuery& query, std::string& error);

    struct PhysicalReadQuery
    {
        std::string   targetName;
        std::uint32_t vmIndex {};
        std::uint64_t gpa {};
        std::uint32_t size {};
    };

    struct PhysicalWriteCommand
    {
        std::string            targetName;
        std::uint32_t          vmIndex {};
        std::uint64_t          gpa {};
        std::vector<std::byte> data;
    };

    struct GpaTranslateQuery
    {
        std::string   targetName;
        std::uint32_t vmIndex {};
        std::uint64_t gpa {};
    };

    struct Virt2PhysQuery
    {
        std::string   targetName;
        std::uint32_t pid {};
        std::uint64_t va {};
    };

    [[nodiscard]] std::optional<std::vector<VmDescriptor>>
    listVms(const std::filesystem::path& socketPath, const std::string& targetName, std::string& error);

    [[nodiscard]] std::optional<std::vector<std::byte>>
    readPhysicalMemory(const std::filesystem::path& socketPath, const PhysicalReadQuery& query, std::string& error);

    [[nodiscard]] std::optional<std::uint64_t> writePhysicalMemory(const std::filesystem::path& socketPath,
                                                                   const PhysicalWriteCommand&  command,
                                                                   std::string&                 error);

    [[nodiscard]] std::optional<GpaTranslateResponse>
    translateGpa(const std::filesystem::path& socketPath, const GpaTranslateQuery& query, std::string& error);

    [[nodiscard]] std::optional<std::uint64_t>
    virt2Phys(const std::filesystem::path& socketPath, const Virt2PhysQuery& query, std::string& error);

    struct PhysicalScatterQuery
    {
        std::string              targetName;
        std::uint32_t            vmIndex {};
        std::vector<ScatterItem> entries;
    };

    [[nodiscard]] std::optional<std::vector<ScatterReadResult>>
    physicalScatter(const std::filesystem::path& socketPath, const PhysicalScatterQuery& query, std::string& error);
} // namespace mpfshost

#endif // MPFSHOST_CORE_CLIENT_CLIENT_LEGACY_HPP
