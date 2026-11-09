#ifndef MPFSHOST_CORE_CLIENT_CLIENT_HPP
#define MPFSHOST_CORE_CLIENT_CLIENT_HPP
#pragma once

#include "core/ipc/message.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mpfshost
{
    namespace detail
    {
        struct Connection;
    } // namespace detail

    inline constexpr std::string_view kDefaultSocketPath = "/run/mpfshost.sock";

    // Client-side failures use the same enum as the wire so callers have one switch to write. The
    // ConnectionFailed, ProtocolError and InvalidArgument values never travel on the wire.
    struct Error
    {
        ErrorCode   code {ErrorCode::Internal};
        std::string message;
    };

    template<class T>
    using Result = std::expected<T, Error>;

    // Every list capability returns the same shape, including whether a server-side cap cut it short.
    template<class T>
    struct List
    {
        std::vector<T> items;
        bool           truncated {};

        [[nodiscard]] bool empty() const noexcept
        {
            return items.empty();
        }

        [[nodiscard]] std::size_t size() const noexcept
        {
            return items.size();
        }

        [[nodiscard]] auto begin() noexcept
        {
            return items.begin();
        }

        [[nodiscard]] auto end() noexcept
        {
            return items.end();
        }

        [[nodiscard]] auto begin() const noexcept
        {
            return items.begin();
        }

        [[nodiscard]] auto end() const noexcept
        {
            return items.end();
        }

        [[nodiscard]] const T& operator[](std::size_t index) const
        {
            return items[index];
        }

        [[nodiscard]] T& operator[](std::size_t index)
        {
            return items[index];
        }
    };

    using Target = TargetDescriptor;

    struct Capabilities
    {
        std::string                serverVersion;
        std::vector<std::uint16_t> requestTypes;

        [[nodiscard]] bool supports(MessageType type) const noexcept;
    };

    struct ScatterRange
    {
        std::uint64_t address {};
        std::uint32_t size {};
    };

    struct ScatterItem
    {
        std::uint64_t          address {};
        std::uint32_t          size {};
        bool                   write {};
        std::vector<std::byte> data;
    };

    struct ScatterReadResult
    {
        std::uint64_t          address {};
        std::uint32_t          requestedSize {};
        std::uint32_t          bytesRead {};
        bool                   ok {};
        std::vector<std::byte> data;
    };

    struct SearchResult
    {
        std::vector<std::uint64_t> addresses;
        bool                       truncated {};
    };

    struct AddressResolution
    {
        bool          moduleFound {};
        std::string   module;
        std::string   moduleFullName;
        std::uint64_t moduleBase {};
        std::uint64_t moduleEntry {};
        std::uint64_t moduleImageSize {};
        std::uint64_t moduleOffset {};
        std::uint16_t moduleKind {};
        bool          moduleWow64 {};
        bool          exportFound {};
        std::uint64_t exportAddress {};
        std::uint64_t exportOffset {};
        std::string   exportName;
        bool          pdbFound {};
        std::string   pdbSymbol;
        std::uint32_t pdbDisplacement {};
    };

    struct DebugWatchOptions
    {
        std::uint32_t    pid {};
        std::uint64_t    address {};
        std::uint32_t    width {4};
        DebugWatchAccess access {DebugWatchAccess::Write};
        std::uint32_t    maxHits {1};
        std::uint32_t    timeoutMs {30000};
        std::string      module;
        std::string      symbol;
    };

    struct DebugBreakpointOptions
    {
        std::uint32_t       pid {};
        std::uint64_t       address {};
        DebugBreakpointKind kind {DebugBreakpointKind::Software};
        std::uint32_t       timeoutMs {30000};
        std::string         module;
        std::string         symbol;
    };

    class Scatter
    {
    public:
        Scatter(Scatter&& other) noexcept;
        Scatter& operator=(Scatter&& other) noexcept;
        ~Scatter();

        Scatter(const Scatter&)            = delete;
        Scatter& operator=(const Scatter&) = delete;

        [[nodiscard]] std::uint32_t id() const noexcept;

        [[nodiscard]] Result<std::uint32_t>                  prepare(const std::vector<ScatterItem>& entries);
        [[nodiscard]] Result<std::vector<ScatterReadResult>> execute();
        [[nodiscard]] Result<std::vector<ScatterReadResult>> executeRead();
        [[nodiscard]] Result<void>                           clear();
        [[nodiscard]] Result<void>                           close();

    private:
        friend class ClientSession;

        Scatter(std::shared_ptr<detail::Connection> connection, std::uint32_t scatterId);

        std::shared_ptr<detail::Connection> _connection;
        std::uint32_t                       _scatterId {};
        bool                                _closed {};
    };

    class ClientSession
    {
    public:
        ClientSession(ClientSession&& other) noexcept;
        ClientSession& operator=(ClientSession&& other) noexcept;
        ~ClientSession();

        ClientSession(const ClientSession&)            = delete;
        ClientSession& operator=(const ClientSession&) = delete;

        [[nodiscard]] const Target&      target() const noexcept;
        [[nodiscard]] const std::string& targetName() const noexcept;

        void close() noexcept;

        [[nodiscard]] Result<List<ProcessDescriptor>> processes(const ProcessFilter& filter = {});
        [[nodiscard]] Result<List<ProcessSummary>>    allProcesses(const ProcessFilter& filter = {});
        [[nodiscard]] Result<ProcessInfoResponse>     processInfo(std::uint32_t pid);
        [[nodiscard]] Result<ProcessInfoResponse>     processInfo(std::string_view name);
        [[nodiscard]] Result<ProcessByDtbResponse>    processByDtb(std::uint64_t dtb);
        [[nodiscard]] Result<List<ModuleDescriptor>>
        modules(std::uint32_t pid, std::string_view nameFilter = {}, std::uint16_t flags = 0);
        [[nodiscard]] Result<SymbolLookupResponse>
        lookupSymbol(std::uint32_t pid, std::string_view module, std::string_view symbol = {});
        [[nodiscard]] Result<PdbLoadResponse>          pdbLoad(std::uint32_t pid, std::uint64_t moduleBase);
        [[nodiscard]] Result<PdbSymbolNameResponse>    pdbSymbolName(std::string_view module, std::uint64_t address);
        [[nodiscard]] Result<PdbSymbolAddressResponse> pdbSymbolAddress(std::string_view module,
                                                                        std::string_view symbol);
        [[nodiscard]] Result<PdbTypeSizeResponse>      pdbTypeSize(std::string_view module, std::string_view type);
        [[nodiscard]] Result<PdbTypeChildOffsetResponse>
        pdbTypeChildOffset(std::string_view module, std::string_view type, std::string_view child);
        [[nodiscard]] Result<ModuleLayoutResponse>    moduleLayout(std::uint32_t pid, std::string_view module);
        [[nodiscard]] Result<ModuleByAddressResponse> moduleByAddress(std::uint32_t pid, std::uint64_t address);
        [[nodiscard]] Result<AddressResolution>       resolveAddress(std::uint32_t pid, std::uint64_t address);
        [[nodiscard]] Result<List<MemoryMapRegion>>   memoryMap(std::uint32_t pid, bool identifyModules = false);
        [[nodiscard]] Result<List<VadExDescriptor>>
        vadEx(std::uint32_t pid, std::uint32_t pageOffset = 0, std::uint32_t pageCount = 0);
        [[nodiscard]] Result<List<ThreadDescriptor>>
        threads(std::uint32_t pid, std::uint32_t tid = 0, std::uint16_t flags = 0);
        [[nodiscard]] Result<List<HandleDescriptor>>         handles(std::uint32_t pid, std::string_view filter = {});
        [[nodiscard]] Result<List<ExportDescriptor>>         exports(std::uint32_t pid, std::string_view module);
        [[nodiscard]] Result<List<ImportDescriptor>>         imports(std::uint32_t pid, std::string_view module);
        [[nodiscard]] Result<List<UnloadedModuleDescriptor>> unloadedModules(std::uint32_t    pid,
                                                                             std::string_view filter = {});
        [[nodiscard]] Result<List<ServiceDescriptor>>        services(std::string_view filter = {});
        [[nodiscard]] Result<List<UserDescriptor>>           users(std::string_view filter = {});
        [[nodiscard]] Result<List<NetworkEntryDescriptor>> network(std::uint32_t pid = 0, std::string_view filter = {});
        [[nodiscard]] Result<List<HeapDescriptor>>         heaps(std::uint32_t pid);
        [[nodiscard]] Result<List<HeapAllocDescriptor>>    heapAllocations(std::uint32_t pid, std::uint64_t heap);
        [[nodiscard]] Result<List<KernelDriverDescriptor>> kernelDrivers(std::string_view filter = {});
        [[nodiscard]] Result<List<KernelDeviceDescriptor>> kernelDevices(std::string_view filter = {});
        [[nodiscard]] Result<List<KernelObjectDescriptor>> kernelObjects(std::string_view filter = {});

        struct PteResult
        {
            List<PteDescriptor> regions;
            std::uint64_t       mapEntries {};
        };

        [[nodiscard]] Result<PteResult> pte(std::uint32_t    pid,
                                            bool             identifyModules = false,
                                            std::string_view filter          = {},
                                            std::uint64_t    minAddress      = 0,
                                            std::uint64_t    maxAddress      = 0);

        struct PoolResult
        {
            List<PoolDescriptor> entries;
            std::uint64_t        mapEntries {};
            std::uint64_t        mapBytes {};
        };

        [[nodiscard]] Result<PoolResult> pool(bool             big        = false,
                                              std::string_view tagFilter  = {},
                                              std::uint32_t    minBytes   = 0,
                                              std::uint64_t    minAddress = 0,
                                              std::uint64_t    maxAddress = 0);

        struct PoolTagResult
        {
            List<PoolTagDescriptor> tags;
            std::uint64_t           mapTags {};
            std::uint64_t           mapEntries {};
        };

        [[nodiscard]] Result<PoolTagResult>       poolTags(bool big = false, std::string_view tagFilter = {});
        [[nodiscard]] Result<List<PfnDescriptor>> pfn(std::uint32_t pfnStart, std::uint32_t pfnCount);
        [[nodiscard]] Result<List<VmDescriptor>>  vms();
        [[nodiscard]] Result<std::uint64_t>      setConfig(ConfigOption option, std::uint32_t pid, std::uint64_t value);
        [[nodiscard]] Result<SearchResult>       searchMemory(SearchTerm    term,
                                                              std::uint32_t pid        = 0,
                                                              std::uint32_t alignBytes = 1,
                                                              std::uint64_t minAddress = 0,
                                                              std::uint64_t maxAddress = 0,
                                                              std::uint32_t maxResults = 0);
        [[nodiscard]] Result<VirtualRegion>      queryVirtual(std::uint32_t pid, std::uint64_t address);
        [[nodiscard]] Result<List<PageReadItem>> readPages(std::uint32_t pid, const std::vector<PageRange>& ranges);
        [[nodiscard]] Result<std::uint32_t>      prefetchPages(std::uint32_t pid, const std::vector<PageRange>& ranges);

        [[nodiscard]] Result<std::vector<std::byte>>
                                   readMemory(std::uint32_t pid, std::uint64_t address, std::uint32_t size);
        [[nodiscard]] Result<void> readMemoryInto(std::uint32_t pid, std::uint64_t address, std::span<std::byte> out);
        [[nodiscard]] Result<std::uint64_t>
        writeMemory(std::uint32_t pid, std::uint64_t address, std::span<const std::byte> data);
        [[nodiscard]] Result<std::vector<ScatterReadResult>> scatterRead(std::uint32_t                    pid,
                                                                         const std::vector<ScatterRange>& ranges);
        [[nodiscard]] Result<Scatter>                        scatter(std::uint32_t pid);

        [[nodiscard]] Result<std::vector<std::byte>>
        readPhysicalMemory(std::uint32_t vmIndex, std::uint64_t gpa, std::uint32_t size);
        [[nodiscard]] Result<std::uint64_t>
        writePhysicalMemory(std::uint32_t vmIndex, std::uint64_t gpa, std::span<const std::byte> data);
        [[nodiscard]] Result<GpaTranslateResponse>           translateGpa(std::uint32_t vmIndex, std::uint64_t gpa);
        [[nodiscard]] Result<std::uint64_t>                  virt2Phys(std::uint32_t pid, std::uint64_t va);
        [[nodiscard]] Result<std::vector<ScatterReadResult>> physicalScatter(std::uint32_t                   vmIndex,
                                                                             const std::vector<ScatterItem>& entries);

        [[nodiscard]] Result<List<VfsEntry>> listVfs(std::string_view path, std::string_view nameFilter = {});
        [[nodiscard]] Result<std::vector<std::byte>>
        readVfs(std::string_view path, std::uint64_t offset, std::uint32_t size);
        [[nodiscard]] Result<std::uint64_t>
        writeVfs(std::string_view path, std::uint64_t offset, std::span<const std::byte> data);

        [[nodiscard]] Result<DebugStatus>                      debugState();
        [[nodiscard]] Result<DebugStatus>                      debugControl(DebugControlAction action);
        [[nodiscard]] Result<std::vector<DebugRegisterRecord>> debugRegisters(std::uint32_t vcpu = kDebugAllVcpus);
        [[nodiscard]] Result<std::vector<DebugThreadRecord>>   debugThreads();
        [[nodiscard]] Result<DisassembleResponse>
        disassemble(std::uint32_t pid, std::uint64_t address, std::uint32_t count = 16);
        [[nodiscard]] Result<DebugBacktraceResponse> debugBacktrace(std::uint32_t    pid,
                                                                    std::uint32_t    vcpu      = 0,
                                                                    std::string_view threadId  = {},
                                                                    std::uint32_t    maxFrames = 32);
        [[nodiscard]] Result<DebugWatchEnd>
        debugWatch(const DebugWatchOptions&                               options,
                   const std::function<void(const DebugWatchData&)>&      onHit,
                   const std::function<void(const DebugWatchHeartbeat&)>& onHeartbeat = {});
        [[nodiscard]] Result<DebugBreakpointResponse> debugBreakpoint(const DebugBreakpointOptions& options);
        [[nodiscard]] Result<DebugStepResponse>       debugStep(std::uint32_t vcpu = 0, std::string_view threadId = {});
        [[nodiscard]] Result<DebugAttachResponse>
        debugAttach(const std::function<void(const DebugAttachResponse&)>& whileAttached = {});
        [[nodiscard]] Result<DebugDetachResponse>         debugDetach();
        [[nodiscard]] Result<DebugBreakpointArmResponse>  debugArmBreakpoint(const DebugBreakpointOptions& options);
        [[nodiscard]] Result<DebugResourceRemoveResponse> debugRemoveBreakpoint(std::uint32_t id);
        [[nodiscard]] Result<List<DebugBreakpointArmResponse>> debugListBreakpoints();
        [[nodiscard]] Result<DebugWatchArmResponse>            debugArmWatch(const DebugWatchOptions& options);
        [[nodiscard]] Result<DebugResourceRemoveResponse>      debugRemoveWatch(std::uint32_t id);
        [[nodiscard]] Result<List<DebugWatchArmResponse>>      debugListWatches();
        [[nodiscard]] Result<DebugStatus>                      debugContinueArmed();
        [[nodiscard]] Result<DebugWaitEnd> debugWait(std::uint32_t                                   timeoutMs,
                                                     std::uint32_t                                   maxHits,
                                                     const std::function<void(const DebugWaitHit&)>& onHit);

    private:
        friend class Client;

        ClientSession(std::shared_ptr<detail::Connection> connection, Target target);

        [[nodiscard]] int fd() const noexcept;

        std::shared_ptr<detail::Connection> _connection;
        Target                              _target;
    };

    class Client
    {
    public:
        explicit Client(std::filesystem::path socketPath = std::filesystem::path {kDefaultSocketPath});

        [[nodiscard]] const std::filesystem::path& socketPath() const noexcept;

        [[nodiscard]] Result<Capabilities>  capabilities() const;
        [[nodiscard]] Result<List<Target>>  targets() const;
        [[nodiscard]] Result<ClientSession> bind(std::string targetName) const;

    private:
        std::filesystem::path _socketPath;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_CLIENT_CLIENT_HPP
