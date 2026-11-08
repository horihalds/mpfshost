#ifndef MPFSHOST_CORE_IPC_MESSAGE_HPP
#define MPFSHOST_CORE_IPC_MESSAGE_HPP
#pragma once

#include "core/ipc/frame.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mpfshost
{
    struct TargetDescriptor
    {
        std::string   name;
        std::uint32_t qemuPid {};
        std::string   qmpSocketPath;
    };

    struct ListTargetsResponse
    {
        std::vector<TargetDescriptor> targets;
    };

    struct CapabilitiesResponse
    {
        std::string                serverVersion;
        std::vector<std::uint16_t> requestTypes;
    };

    struct BindRequest
    {
        std::string targetName;
    };

    struct BindResponse
    {
        std::uint64_t    sessionId {};
        TargetDescriptor target;
    };

    struct ErrorResponse
    {
        ErrorCode   code {ErrorCode::Internal};
        std::string message;
    };

    struct ProcessDescriptor
    {
        std::uint32_t pid {};
        std::uint32_t parentPid {};
        std::string   name;
        bool          wow64 {};
    };

    struct ProcessFilter
    {
        std::uint32_t pid {};     // 0 selects every process
        std::string   nameFilter; // empty selects every process
    };

    struct ProcessListResponse
    {
        std::vector<ProcessDescriptor> processes;
    };

    struct ProcessSummary
    {
        ProcessDescriptor process;
        std::uint32_t     state {}; // VMMDLL_PROCESS_INFORMATION::dwState
    };

    struct ProcessListAllResponse
    {
        std::vector<ProcessSummary> processes;
    };

    struct MemoryReadRequest
    {
        std::uint32_t pid {};
        std::uint64_t address {};
        std::uint32_t size {};
    };

    struct MemoryReadEnd
    {
        std::uint8_t  status {}; // 0 = ok, 1 = failed
        std::uint64_t bytesDelivered {};
        std::string   error;
    };

    struct MemoryWriteRequest
    {
        std::uint32_t          pid {};
        std::uint64_t          address {};
        std::vector<std::byte> data;
    };

    struct MemoryWriteResponse
    {
        std::uint32_t bytesWritten {};
    };

    inline constexpr std::uint8_t kScatterEntryRead  = 0;
    inline constexpr std::uint8_t kScatterEntryWrite = 1;

    struct ScatterEntry
    {
        std::uint64_t          address {};
        std::uint32_t          size {};
        std::uint8_t           kind {kScatterEntryRead};
        std::vector<std::byte> data;
    };

    struct ScatterOutcome
    {
        std::uint64_t address {};
        std::uint32_t requestedSize {};
        std::uint32_t bytesRead {};
        std::uint8_t  ok {};
    };

    struct ScatterCreateRequest
    {
        std::uint32_t pid {};
    };

    struct ScatterCreateResponse
    {
        std::uint32_t scatterId {};
    };

    struct ScatterPrepareRequest
    {
        std::uint32_t             scatterId {};
        std::vector<ScatterEntry> entries;
    };

    struct ScatterPrepareResponse
    {
        std::uint32_t entryCount {};
    };

    struct ScatterExecuteRequest
    {
        std::uint32_t scatterId {};
    };

    struct ScatterExecuteData
    {
        std::uint32_t          entryIndex {};
        std::vector<std::byte> data;
    };

    struct ScatterExecuteEnd
    {
        std::uint8_t                status {}; // 0 = ok, 1 = failed
        std::vector<ScatterOutcome> outcomes;
        std::string                 error;
    };

    struct ScatterCloseRequest
    {
        std::uint32_t scatterId {};
    };

    struct ScatterCloseResponse
    {
        std::uint32_t scatterId {};
    };

    struct ScatterExecuteReadRequest
    {
        std::uint32_t scatterId {};
    };

    using ScatterExecuteReadData = ScatterExecuteData;
    using ScatterExecuteReadEnd  = ScatterExecuteEnd;

    struct ScatterClearRequest
    {
        std::uint32_t scatterId {};
    };

    struct ScatterClearResponse
    {
        std::uint32_t scatterId {};
    };

    inline constexpr std::uint16_t kModuleFlagCounts      = 0x0001;
    inline constexpr std::uint16_t kModuleFlagDebugInfo   = 0x0002;
    inline constexpr std::uint16_t kModuleFlagVersionInfo = 0x0004;

    struct ModuleListRequest
    {
        std::uint32_t pid {};
        std::uint16_t flags {};   // kModuleFlag*; any other bit is rejected at decode
        std::string   nameFilter; // empty selects every module
    };

    struct ModuleDescriptor
    {
        std::uint64_t base {};
        std::uint64_t entry {};
        std::uint32_t imageSize {};
        std::uint32_t rawFileSize {};  // kModuleFlagCounts
        std::uint16_t kind {};         // VMMDLL_MODULE_TP
        std::uint16_t sectionCount {}; // kModuleFlagCounts
        std::uint32_t eatCount {};     // kModuleFlagCounts
        std::uint32_t iatCount {};     // kModuleFlagCounts
        bool          wow64 {};
        std::string   name;
        std::string   fullName;
        std::string   pdbGuid; // kModuleFlagDebugInfo
        std::string   pdbName; // kModuleFlagDebugInfo
        std::string   version; // kModuleFlagVersionInfo
    };

    struct ModuleListData
    {
        std::vector<ModuleDescriptor> modules;
    };

    struct ModuleListEnd
    {
        std::uint8_t  status {}; // 0 = ok, 1 = failed
        std::uint64_t moduleCount {};
        std::string   error;
    };

    inline constexpr std::uint16_t kMemoryMapFlagImage    = 0x0001;
    inline constexpr std::uint16_t kMemoryMapFlagFile     = 0x0002;
    inline constexpr std::uint16_t kMemoryMapFlagPageFile = 0x0004;
    inline constexpr std::uint16_t kMemoryMapFlagPrivate  = 0x0008;
    inline constexpr std::uint16_t kMemoryMapFlagTeb      = 0x0010;
    inline constexpr std::uint16_t kMemoryMapFlagStack    = 0x0020;
    inline constexpr std::uint16_t kMemoryMapFlagHeap     = 0x0040;

    struct MemoryMapRequest
    {
        std::uint32_t pid {};
        bool          identifyModules {}; // MemProcFS fIdentifyModules; slower, richer descriptions
    };

    struct MemoryMapRegion
    {
        std::uint64_t start {};
        std::uint64_t end {}; // inclusive vaEnd, as MemProcFS reports it
        std::uint32_t protection {};
        std::uint32_t commitCharge {};
        std::uint16_t vadType {};
        std::uint16_t flags {}; // kMemoryMapFlag*
        std::uint8_t  heapNumber {};
        std::string   description;
    };

    struct MemoryMapData
    {
        std::vector<MemoryMapRegion> regions;
    };

    struct MemoryMapEnd
    {
        std::uint8_t  status {}; // 0 = ok, 1 = failed
        std::uint64_t regionCount {};
        std::string   error;
    };

    inline constexpr std::uint8_t kVadExFlagHardware  = 0x01;
    inline constexpr std::uint8_t kVadExFlagWrite     = 0x10;
    inline constexpr std::uint8_t kVadExFlagKernel    = 0x40;
    inline constexpr std::uint8_t kVadExFlagNoExecute = 0x80;

    struct VadExListRequest
    {
        std::uint32_t pid {};        // must be non-zero; VadEx is per process
        std::uint32_t pageOffset {}; // oPage: offset in pages from the process start
        std::uint32_t pageCount {};  // cPage: number of pages to describe
    };

    struct VadExDescriptor
    {
        std::uint32_t type {};      // VMMDLL_PTE_TP of the page
        std::uint8_t  pml {};       // iPML, the page-map level that resolved the page
        std::uint8_t  flags {};     // kVadExFlag*
        std::uint64_t va {};        // virtual address of the page
        std::uint64_t pa {};        // physical address of the page
        std::uint64_t pte {};       // the page-table entry value
        std::uint32_t protoType {}; // VMMDLL_PTE_TP of the prototype PTE
        std::uint64_t protoPa {};   // physical address of the prototype PTE
        std::uint64_t protoPte {};  // the prototype PTE value
        std::uint64_t vaVadBase {}; // base address of the owning VAD
    };

    struct VadExListData
    {
        std::vector<VadExDescriptor> entries;
    };

    struct VadExListEnd
    {
        std::uint8_t  status {}; // 0 = ok, 1 = failed
        std::uint64_t entryCount {};
        std::string   error;
    };

    inline constexpr std::uint16_t kThreadFlagCallstack = 0x0001;
    inline constexpr std::size_t   kMaxCallstackFrames  = 64;

    struct ThreadListRequest
    {
        std::uint32_t pid {};
        std::uint32_t tid {};   // 0 selects every thread of the process
        std::uint16_t flags {}; // kThreadFlag*; any other bit is rejected at decode
    };

    struct ThreadFrame
    {
        std::uint32_t index {};
        std::uint64_t returnAddress {};
        std::uint64_t stackPointer {};
        std::uint64_t frameBase {};    // vaBaseSP
        std::uint32_t displacement {}; // cbDisplacement
        bool          registersPresent {};
        std::string   module;
        std::string   function;
    };

    struct ThreadDescriptor
    {
        std::uint32_t            tid {};
        std::uint32_t            pid {};
        std::uint32_t            exitStatus {};
        std::uint32_t            userTime {};   // dwUserTime
        std::uint32_t            kernelTime {}; // dwKernelTime
        std::uint64_t            ethread {};
        std::uint64_t            teb {};
        std::uint64_t            startAddress {};
        std::uint64_t            win32StartAddress {};
        std::uint64_t            stackBaseUser {};
        std::uint64_t            stackLimitUser {};
        std::uint64_t            stackBaseKernel {};
        std::uint64_t            stackLimitKernel {};
        std::uint64_t            trapFrame {};
        std::uint64_t            instructionPointer {}; // vaRIP, valid for user-mode threads
        std::uint64_t            stackPointer {};       // vaRSP
        std::uint64_t            affinity {};
        std::uint64_t            createTime {}; // ftCreateTime
        std::uint64_t            exitTime {};
        std::uint64_t            impersonationToken {};
        std::uint8_t             state {};
        bool                     running {};
        std::uint8_t             priority {};
        std::uint8_t             basePriority {};
        std::uint8_t             suspendCount {};
        std::uint8_t             waitReason {};
        std::uint8_t             callstackStatus {}; // 0 = ok or not requested, 1 = retrieval failed
        std::vector<ThreadFrame> frames;             // kThreadFlagCallstack only
    };

    struct ThreadListData
    {
        std::vector<ThreadDescriptor> threads;
    };

    struct ThreadListEnd
    {
        std::uint8_t  status {}; // 0 = ok, 1 = failed
        std::uint64_t threadCount {};
        std::string   error;
    };

    struct VmDescriptor
    {
        std::string   name; // uszName, may be empty
        std::uint64_t gpaMax {};
        std::uint16_t kind {};       // VMMDLL_VM_TP
        std::uint16_t systemKind {}; // VMMDLL_SYSTEM_TP
        std::uint32_t partitionId {};
        std::uint32_t versionBuild {};
        std::uint32_t vmMemPid {}; // dwVmMemPID, the 'vmmem' process mapping guest RAM
        bool          active {};
        bool          readOnly {};
        bool          physicalOnly {};
    };

    struct VmListResponse
    {
        std::vector<VmDescriptor> vms;
    };

    struct PhysicalReadRequest
    {
        std::uint32_t vmIndex {};
        std::uint64_t gpa {};
        std::uint32_t size {};
    };

    struct PhysicalReadEnd
    {
        std::uint8_t  status {}; // 0 = ok, 1 = failed
        std::uint64_t bytesDelivered {};
        std::string   error;
    };

    struct PhysicalWriteRequest
    {
        std::uint32_t          vmIndex {};
        std::uint64_t          gpa {};
        std::vector<std::byte> data;
    };

    struct GpaTranslateRequest
    {
        std::uint32_t vmIndex {};
        std::uint64_t gpa {};
    };

    struct GpaTranslateResponse
    {
        std::uint64_t pa {};
        std::uint64_t va {};
        bool          paPresent {};
        bool          vaPresent {};
    };

    struct Virt2PhysRequest
    {
        std::uint32_t pid {};
        std::uint64_t va {};
    };

    struct Virt2PhysResponse
    {
        std::uint64_t pa {};
    };

    struct PhysicalScatterRequest
    {
        std::uint32_t             vmIndex {};
        std::vector<ScatterEntry> entries;
    };

    struct VfsListRequest
    {
        std::string path;       // empty selects the tree root
        std::string nameFilter; // empty selects every entry
    };

    struct VfsEntry
    {
        std::string   name;
        std::uint64_t size {};
        std::uint64_t creationTime {};
        std::uint64_t lastAccessTime {};
        std::uint64_t lastWriteTime {};
        bool          isDirectory {};
    };

    struct VfsListData
    {
        std::vector<VfsEntry> entries;
    };

    struct VfsListEnd
    {
        std::uint8_t  status {}; // 0 = ok, 1 = failed
        std::uint64_t entryCount {};
        std::string   error;
    };

    struct VfsReadRequest
    {
        std::string   path;
        std::uint64_t offset {};
        std::uint32_t size {};
    };

    struct VfsReadEnd
    {
        std::uint8_t  status {}; // 0 = ok, 1 = failed
        std::uint64_t bytesRead {};
        std::string   error;
    };

    struct VfsWriteRequest
    {
        std::string            path;
        std::uint64_t          offset {};
        std::vector<std::byte> data;
    };

    struct VfsWriteResponse
    {
        std::uint32_t bytesWritten {};
    };

    struct ProcessInfoRequest
    {
        std::uint32_t pid {}; // 0 resolves through `name`
        std::string   name;
    };

    struct ProcessInfoResponse
    {
        ProcessDescriptor process;
        std::uint32_t     state {};       // VMMDLL_PROCESS_INFORMATION::dwState
        std::uint16_t     memoryModel {}; // VMMDLL_MEMORYMODEL_TP
        std::uint16_t     systemType {};  // VMMDLL_SYSTEM_TP
        bool              userOnly {};    // fUserOnly
        std::uint64_t     dtb {};
        std::uint64_t     dtbUser {}; // paDTB_UserOpt
        std::uint64_t     eprocessVa {};
        std::uint64_t     pebVa {};
        std::uint32_t     peb32Va {}; // WoW64 only
        std::uint32_t     sessionId {};
        std::uint64_t     luid {};
        std::uint32_t     integrityLevel {}; // VMMDLL_PROCESS_INTEGRITY_LEVEL
        std::string       sid;
        std::string       imagePath;  // VMMDLL_PROCESS_INFORMATION_OPT_STRING_PATH_USER_IMAGE
        std::string       kernelPath; // VMMDLL_PROCESS_INFORMATION_OPT_STRING_PATH_KERNEL
        std::string       cmdline;    // VMMDLL_PROCESS_INFORMATION_OPT_STRING_CMDLINE
    };

    struct ProcessByDtbRequest
    {
        std::uint64_t dtb {}; // kernel or user directory table base; low CR3 flag bits are ignored
    };

    struct ProcessByDtbResponse
    {
        ProcessDescriptor process;
        std::uint64_t     dtb {};         // the process's paDTB
        std::uint64_t     dtbUser {};     // paDTB_UserOpt, zero when MemProcFS has none
        bool              matchedUser {}; // true when the request matched the user DTB
    };

    // The wire form of QEMU's run state. States that have no dedicated value are reported as Other, so the
    // enum stays stable when QEMU adds one.
    enum class DebugRunState : std::uint8_t
    {
        Unknown = 0,
        Running,
        Paused,
        Shutdown,
        Suspended,
        GuestPanicked,
        Other,
    };

    [[nodiscard]] std::string_view debugRunStateName(DebugRunState state) noexcept;

    enum class DebugControlAction : std::uint8_t
    {
        Stop     = 0,
        Continue = 1,
    };

    struct DebugStateRequest
    {
        // Empty payload; the target comes from the binding.
    };

    struct DebugControlRequest
    {
        DebugControlAction action {DebugControlAction::Stop};
    };

    struct DebugStatus
    {
        DebugRunState runState {DebugRunState::Unknown};
        bool          stubActive {};
        bool          hostPaused {}; // the host pressed stop and has not continued it
        std::uint32_t vcpuCount {};
        std::uint32_t watchCount {};
    };

    using DebugStateResponse   = DebugStatus;
    using DebugControlResponse = DebugStatus;

    struct SymbolLookupRequest
    {
        std::uint32_t pid {};
        std::string   module;
        std::string   symbol; // empty returns the module base only
    };

    struct SymbolLookupResponse
    {
        std::uint64_t moduleBase {};
        std::uint64_t address {}; // zero with found == false when the symbol is missing
        std::uint64_t offset {};  // address - moduleBase
        bool          found {};
    };

    struct PdbLoadRequest
    {
        std::uint32_t pid {};
        std::uint64_t moduleBase {};
    };

    struct PdbLoadResponse
    {
        std::string module; // module name associated with the loaded pdb
    };

    struct PdbSymbolNameRequest
    {
        std::string   module;
        std::uint64_t address {}; // symbol virtual address or module-relative offset
    };

    struct PdbSymbolNameResponse
    {
        std::string   symbol;
        std::uint32_t displacement {}; // bytes past the start of the symbol
    };

    struct PdbSymbolAddressRequest
    {
        std::string module;
        std::string symbol;
    };

    struct PdbSymbolAddressResponse
    {
        std::uint64_t address {};
    };

    struct PdbTypeSizeRequest
    {
        std::string module;
        std::string type;
    };

    struct PdbTypeSizeResponse
    {
        std::uint32_t size {};
    };

    struct PdbTypeChildOffsetRequest
    {
        std::string module;
        std::string type;
        std::string child;
    };

    struct PdbTypeChildOffsetResponse
    {
        std::uint32_t offset {};
    };

    inline constexpr std::size_t kMaxModuleSections        = 96;
    inline constexpr std::size_t kModuleDataDirectoryCount = 16;

    struct ModuleSection
    {
        std::string   name; // IMAGE_SECTION_HEADER.Name, trimmed at the first NUL
        std::uint64_t virtualAddress {};
        std::uint32_t virtualSize {};
        std::uint32_t rawPointer {};
        std::uint32_t rawSize {};
        std::uint32_t characteristics {};
    };

    struct ModuleDataDirectory
    {
        std::uint32_t index {};
        std::uint32_t virtualAddress {};
        std::uint32_t size {};
    };

    struct ModuleLayoutRequest
    {
        std::uint32_t pid {};
        std::string   module;
    };

    struct ModuleLayoutResponse
    {
        std::uint64_t                    moduleBase {};
        std::vector<ModuleSection>       sections;
        std::vector<ModuleDataDirectory> directories; // always kModuleDataDirectoryCount entries
    };

    struct ModuleByAddressRequest
    {
        std::uint32_t pid {};
        std::uint64_t address {};
    };

    struct ModuleByAddressResponse
    {
        std::uint64_t base {};
        std::uint64_t entry {};
        std::uint64_t imageSize {};
        std::uint64_t offset {}; // address - base
        std::uint16_t kind {};
        bool          wow64 {};
        std::string   name;
        std::string   fullName;
    };

    struct CancelRequest
    {
        std::uint64_t requestId {};
    };

    struct CancelResponse
    {
        std::uint8_t  cancelled {};
        std::uint64_t requestId {};
    };

    // A VMMDLL_MEMORY_BASIC_INFORMATION in wire form. Fields that MemProcFS leaves as an ABI placeholder
    // are not transmitted.
    struct VirtualRegion
    {
        std::uint64_t baseAddress {};
        std::uint64_t allocationBase {};
        std::uint64_t regionSize {};
        std::uint32_t allocationProtect {};
        std::uint32_t state {};
        std::uint32_t protect {};
        std::uint32_t type {};
        std::uint32_t partitionId {};
    };

    struct VirtualQueryRequest
    {
        std::uint32_t pid {};
        std::uint64_t address {};
    };

    struct VirtualQueryResponse
    {
        VirtualRegion region;
    };

    struct PageRange
    {
        std::uint64_t address {};
        std::uint64_t size {};
    };

    struct PageReadRequest
    {
        std::uint32_t          pid {};
        std::vector<PageRange> ranges;
    };

    // One 4 KiB page. `ok` is false when MemProcFS could not read it, which is a per-item answer rather than a
    // request failure.
    struct PageReadItem
    {
        std::uint64_t          address {};
        bool                   ok {};
        std::vector<std::byte> data;
    };

    struct PageReadData
    {
        std::vector<PageReadItem> pages;
    };

    struct PageReadEnd
    {
        std::uint8_t  status {};
        std::uint32_t pageCount {};
        std::string   error;
    };

    struct PrefetchRequest
    {
        std::uint32_t          pid {};
        std::vector<PageRange> ranges;
    };

    struct PrefetchResponse
    {
        std::uint32_t pageCount {};
    };

    // `mask` is 0x00 where the pattern byte must match and 0xFF where it is a wildcard; both vectors always
    // have the same length as the pattern.
    struct SearchTerm
    {
        std::vector<std::byte> pattern;
        std::vector<std::byte> mask;
    };

    struct MemorySearchRequest
    {
        std::uint32_t pid {};
        SearchTerm    term;
        std::uint32_t alignBytes {1};
        std::uint64_t minAddress {}; // rounded down to a page by the host
        std::uint64_t maxAddress {}; // 0 = to the end of the address space
        std::uint32_t maxResults {};
    };

    struct MemorySearchData
    {
        std::vector<std::uint64_t> addresses;
    };

    struct MemorySearchEnd
    {
        std::uint8_t  status {}; // 0 = ok, 1 = failed
        std::uint32_t hitCount {};
        std::uint8_t  truncated {};
        std::string   error;
    };

    [[nodiscard]] std::vector<std::byte>             encodeListTargetsResponse(const ListTargetsResponse& message);
    [[nodiscard]] std::optional<ListTargetsResponse> decodeListTargetsResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>              encodeCapabilitiesResponse(const CapabilitiesResponse& message);
    [[nodiscard]] std::optional<CapabilitiesResponse> decodeCapabilitiesResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>     encodeBindRequest(const BindRequest& message);
    [[nodiscard]] std::optional<BindRequest> decodeBindRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>      encodeBindResponse(const BindResponse& message);
    [[nodiscard]] std::optional<BindResponse> decodeBindResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>       encodeErrorResponse(const ErrorResponse& message);
    [[nodiscard]] std::optional<ErrorResponse> decodeErrorResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>       encodeProcessListRequest(const ProcessFilter& filter);
    [[nodiscard]] std::optional<ProcessFilter> decodeProcessListRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>             encodeProcessListResponse(const ProcessListResponse& message);
    [[nodiscard]] std::optional<ProcessListResponse> decodeProcessListResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte> encodeProcessListAllResponse(const ProcessListAllResponse& message);
    [[nodiscard]] std::optional<ProcessListAllResponse>
    decodeProcessListAllResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>           encodeMemoryReadRequest(const MemoryReadRequest& message);
    [[nodiscard]] std::optional<MemoryReadRequest> decodeMemoryReadRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>       encodeMemoryReadEnd(const MemoryReadEnd& message);
    [[nodiscard]] std::optional<MemoryReadEnd> decodeMemoryReadEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>            encodeMemoryWriteRequest(const MemoryWriteRequest& message);
    [[nodiscard]] std::optional<MemoryWriteRequest> decodeMemoryWriteRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>             encodeMemoryWriteResponse(const MemoryWriteResponse& message);
    [[nodiscard]] std::optional<MemoryWriteResponse> decodeMemoryWriteResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>              encodeScatterCreateRequest(const ScatterCreateRequest& message);
    [[nodiscard]] std::optional<ScatterCreateRequest> decodeScatterCreateRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte> encodeScatterCreateResponse(const ScatterCreateResponse& message);
    [[nodiscard]] std::optional<ScatterCreateResponse> decodeScatterCreateResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte> encodeScatterPrepareRequest(const ScatterPrepareRequest& message);
    [[nodiscard]] std::optional<ScatterPrepareRequest> decodeScatterPrepareRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte> encodeScatterPrepareResponse(const ScatterPrepareResponse& message);
    [[nodiscard]] std::optional<ScatterPrepareResponse>
    decodeScatterPrepareResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte> encodeScatterExecuteRequest(const ScatterExecuteRequest& message);
    [[nodiscard]] std::optional<ScatterExecuteRequest> decodeScatterExecuteRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>            encodeScatterExecuteData(const ScatterExecuteData& message);
    [[nodiscard]] std::optional<ScatterExecuteData> decodeScatterExecuteData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>           encodeScatterExecuteEnd(const ScatterExecuteEnd& message);
    [[nodiscard]] std::optional<ScatterExecuteEnd> decodeScatterExecuteEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>             encodeScatterCloseRequest(const ScatterCloseRequest& message);
    [[nodiscard]] std::optional<ScatterCloseRequest> decodeScatterCloseRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>              encodeScatterCloseResponse(const ScatterCloseResponse& message);
    [[nodiscard]] std::optional<ScatterCloseResponse> decodeScatterCloseResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte> encodeScatterExecuteReadRequest(const ScatterExecuteReadRequest& message);
    [[nodiscard]] std::optional<ScatterExecuteReadRequest>
    decodeScatterExecuteReadRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte> encodeScatterExecuteReadData(const ScatterExecuteReadData& message);
    [[nodiscard]] std::optional<ScatterExecuteReadData>
    decodeScatterExecuteReadData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte> encodeScatterExecuteReadEnd(const ScatterExecuteReadEnd& message);
    [[nodiscard]] std::optional<ScatterExecuteReadEnd> decodeScatterExecuteReadEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>             encodeScatterClearRequest(const ScatterClearRequest& message);
    [[nodiscard]] std::optional<ScatterClearRequest> decodeScatterClearRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>              encodeScatterClearResponse(const ScatterClearResponse& message);
    [[nodiscard]] std::optional<ScatterClearResponse> decodeScatterClearResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>           encodeModuleListRequest(const ModuleListRequest& message);
    [[nodiscard]] std::optional<ModuleListRequest> decodeModuleListRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>        encodeModuleListData(const ModuleListData& message);
    [[nodiscard]] std::optional<ModuleListData> decodeModuleListData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>       encodeModuleListEnd(const ModuleListEnd& message);
    [[nodiscard]] std::optional<ModuleListEnd> decodeModuleListEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>          encodeMemoryMapRequest(const MemoryMapRequest& message);
    [[nodiscard]] std::optional<MemoryMapRequest> decodeMemoryMapRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>       encodeMemoryMapData(const MemoryMapData& message);
    [[nodiscard]] std::optional<MemoryMapData> decodeMemoryMapData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>      encodeMemoryMapEnd(const MemoryMapEnd& message);
    [[nodiscard]] std::optional<MemoryMapEnd> decodeMemoryMapEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>          encodeVadExListRequest(const VadExListRequest& message);
    [[nodiscard]] std::optional<VadExListRequest> decodeVadExListRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>       encodeVadExListData(const VadExListData& message);
    [[nodiscard]] std::optional<VadExListData> decodeVadExListData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>      encodeVadExListEnd(const VadExListEnd& message);
    [[nodiscard]] std::optional<VadExListEnd> decodeVadExListEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>           encodeThreadListRequest(const ThreadListRequest& message);
    [[nodiscard]] std::optional<ThreadListRequest> decodeThreadListRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>        encodeThreadListData(const ThreadListData& message);
    [[nodiscard]] std::optional<ThreadListData> decodeThreadListData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>       encodeThreadListEnd(const ThreadListEnd& message);
    [[nodiscard]] std::optional<ThreadListEnd> decodeThreadListEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>        encodeVmListResponse(const VmListResponse& message);
    [[nodiscard]] std::optional<VmListResponse> decodeVmListResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>             encodePhysicalReadRequest(const PhysicalReadRequest& message);
    [[nodiscard]] std::optional<PhysicalReadRequest> decodePhysicalReadRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>         encodePhysicalReadEnd(const PhysicalReadEnd& message);
    [[nodiscard]] std::optional<PhysicalReadEnd> decodePhysicalReadEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>              encodePhysicalWriteRequest(const PhysicalWriteRequest& message);
    [[nodiscard]] std::optional<PhysicalWriteRequest> decodePhysicalWriteRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>             encodeGpaTranslateRequest(const GpaTranslateRequest& message);
    [[nodiscard]] std::optional<GpaTranslateRequest> decodeGpaTranslateRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>              encodeGpaTranslateResponse(const GpaTranslateResponse& message);
    [[nodiscard]] std::optional<GpaTranslateResponse> decodeGpaTranslateResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>          encodeVirt2PhysRequest(const Virt2PhysRequest& message);
    [[nodiscard]] std::optional<Virt2PhysRequest> decodeVirt2PhysRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>           encodeVirt2PhysResponse(const Virt2PhysResponse& message);
    [[nodiscard]] std::optional<Virt2PhysResponse> decodeVirt2PhysResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte> encodePhysicalScatterRequest(const PhysicalScatterRequest& message);
    [[nodiscard]] std::optional<PhysicalScatterRequest>
    decodePhysicalScatterRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>        encodeVfsListRequest(const VfsListRequest& message);
    [[nodiscard]] std::optional<VfsListRequest> decodeVfsListRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>     encodeVfsListData(const VfsListData& message);
    [[nodiscard]] std::optional<VfsListData> decodeVfsListData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>    encodeVfsListEnd(const VfsListEnd& message);
    [[nodiscard]] std::optional<VfsListEnd> decodeVfsListEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>        encodeVfsReadRequest(const VfsReadRequest& message);
    [[nodiscard]] std::optional<VfsReadRequest> decodeVfsReadRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>    encodeVfsReadEnd(const VfsReadEnd& message);
    [[nodiscard]] std::optional<VfsReadEnd> decodeVfsReadEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>         encodeVfsWriteRequest(const VfsWriteRequest& message);
    [[nodiscard]] std::optional<VfsWriteRequest> decodeVfsWriteRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>          encodeVfsWriteResponse(const VfsWriteResponse& message);
    [[nodiscard]] std::optional<VfsWriteResponse> decodeVfsWriteResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>            encodeProcessInfoRequest(const ProcessInfoRequest& message);
    [[nodiscard]] std::optional<ProcessInfoRequest> decodeProcessInfoRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>             encodeProcessInfoResponse(const ProcessInfoResponse& message);
    [[nodiscard]] std::optional<ProcessInfoResponse> decodeProcessInfoResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>             encodeProcessByDtbRequest(const ProcessByDtbRequest& message);
    [[nodiscard]] std::optional<ProcessByDtbRequest> decodeProcessByDtbRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>              encodeProcessByDtbResponse(const ProcessByDtbResponse& message);
    [[nodiscard]] std::optional<ProcessByDtbResponse> decodeProcessByDtbResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>             encodeSymbolLookupRequest(const SymbolLookupRequest& message);
    [[nodiscard]] std::optional<SymbolLookupRequest> decodeSymbolLookupRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>              encodeSymbolLookupResponse(const SymbolLookupResponse& message);
    [[nodiscard]] std::optional<SymbolLookupResponse> decodeSymbolLookupResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>        encodePdbLoadRequest(const PdbLoadRequest& message);
    [[nodiscard]] std::optional<PdbLoadRequest> decodePdbLoadRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>         encodePdbLoadResponse(const PdbLoadResponse& message);
    [[nodiscard]] std::optional<PdbLoadResponse> decodePdbLoadResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>              encodePdbSymbolNameRequest(const PdbSymbolNameRequest& message);
    [[nodiscard]] std::optional<PdbSymbolNameRequest> decodePdbSymbolNameRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte> encodePdbSymbolNameResponse(const PdbSymbolNameResponse& message);
    [[nodiscard]] std::optional<PdbSymbolNameResponse> decodePdbSymbolNameResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte> encodePdbSymbolAddressRequest(const PdbSymbolAddressRequest& message);
    [[nodiscard]] std::optional<PdbSymbolAddressRequest>
    decodePdbSymbolAddressRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte> encodePdbSymbolAddressResponse(const PdbSymbolAddressResponse& message);
    [[nodiscard]] std::optional<PdbSymbolAddressResponse>
    decodePdbSymbolAddressResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>            encodePdbTypeSizeRequest(const PdbTypeSizeRequest& message);
    [[nodiscard]] std::optional<PdbTypeSizeRequest> decodePdbTypeSizeRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>             encodePdbTypeSizeResponse(const PdbTypeSizeResponse& message);
    [[nodiscard]] std::optional<PdbTypeSizeResponse> decodePdbTypeSizeResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte> encodePdbTypeChildOffsetRequest(const PdbTypeChildOffsetRequest& message);
    [[nodiscard]] std::optional<PdbTypeChildOffsetRequest>
    decodePdbTypeChildOffsetRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte> encodePdbTypeChildOffsetResponse(const PdbTypeChildOffsetResponse& message);
    [[nodiscard]] std::optional<PdbTypeChildOffsetResponse>
    decodePdbTypeChildOffsetResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>             encodeModuleLayoutRequest(const ModuleLayoutRequest& message);
    [[nodiscard]] std::optional<ModuleLayoutRequest> decodeModuleLayoutRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>              encodeModuleLayoutResponse(const ModuleLayoutResponse& message);
    [[nodiscard]] std::optional<ModuleLayoutResponse> decodeModuleLayoutResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte> encodeModuleByAddressRequest(const ModuleByAddressRequest& message);
    [[nodiscard]] std::optional<ModuleByAddressRequest>
    decodeModuleByAddressRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte> encodeModuleByAddressResponse(const ModuleByAddressResponse& message);
    [[nodiscard]] std::optional<ModuleByAddressResponse>
    decodeModuleByAddressResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>       encodeCancelRequest(const CancelRequest& message);
    [[nodiscard]] std::optional<CancelRequest> decodeCancelRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>        encodeCancelResponse(const CancelResponse& message);
    [[nodiscard]] std::optional<CancelResponse> decodeCancelResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>             encodeVirtualQueryRequest(const VirtualQueryRequest& message);
    [[nodiscard]] std::optional<VirtualQueryRequest> decodeVirtualQueryRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>              encodeVirtualQueryResponse(const VirtualQueryResponse& message);
    [[nodiscard]] std::optional<VirtualQueryResponse> decodeVirtualQueryResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>         encodePageReadRequest(const PageReadRequest& message);
    [[nodiscard]] std::optional<PageReadRequest> decodePageReadRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>      encodePageReadData(const PageReadData& message);
    [[nodiscard]] std::optional<PageReadData> decodePageReadData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>     encodePageReadEnd(const PageReadEnd& message);
    [[nodiscard]] std::optional<PageReadEnd> decodePageReadEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>         encodePrefetchRequest(const PrefetchRequest& message);
    [[nodiscard]] std::optional<PrefetchRequest> decodePrefetchRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>          encodePrefetchResponse(const PrefetchResponse& message);
    [[nodiscard]] std::optional<PrefetchResponse> decodePrefetchResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>             encodeMemorySearchRequest(const MemorySearchRequest& message);
    [[nodiscard]] std::optional<MemorySearchRequest> decodeMemorySearchRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>          encodeMemorySearchData(const MemorySearchData& message);
    [[nodiscard]] std::optional<MemorySearchData> decodeMemorySearchData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>         encodeMemorySearchEnd(const MemorySearchEnd& message);
    [[nodiscard]] std::optional<MemorySearchEnd> decodeMemorySearchEnd(std::span<const std::byte> payload);

    struct HandleListRequest
    {
        std::uint32_t pid {};
        std::string   filter;
    };

    struct HandleDescriptor
    {
        std::uint64_t vaObject {};
        std::uint32_t handle {};
        std::uint32_t grantedAccess {}; // dwGrantedAccess, 24 bits wide in the map entry
        std::uint8_t  typeIndex {};     // iType, 8 bits wide in the map entry
        std::uint64_t handleCount {};
        std::uint64_t pointerCount {};
        std::uint64_t objectCreateInfo {};
        std::uint64_t securityDescriptor {};
        std::uint32_t poolTag {};
        std::string   text; // uszText, may be empty
        std::string   type; // uszType, may be empty
    };

    struct HandleListData
    {
        std::vector<HandleDescriptor> handles;
    };

    struct HandleListEnd
    {
        std::uint8_t  status {};
        std::uint64_t handleCount {};
        std::uint8_t  truncated {};
        std::string   error;
    };

    struct ExportListRequest
    {
        std::uint32_t pid {};
        std::string   module;
    };

    struct ExportDescriptor
    {
        std::uint64_t vaFunction {};
        std::uint32_t ordinal {};
        std::uint32_t functionsArrayOffset {}; // oFunctionsArray
        std::uint32_t namesArrayOffset {};     // oNamesArray
        std::string   function;                // uszFunction, may be empty
        std::string   forwardedFunction;       // uszForwardedFunction, may be empty
    };

    struct ExportListData
    {
        std::vector<ExportDescriptor> exports;
    };

    struct ExportListEnd
    {
        std::uint8_t  status {};
        std::uint64_t exportCount {};
        std::uint8_t  truncated {};
        std::string   error;
    };

    struct ImportListRequest
    {
        std::uint32_t pid {};
        std::string   module;
    };

    struct ImportDescriptor
    {
        std::uint64_t vaFunction {};
        bool          is32Bit {};               // Thunk.f32
        std::uint16_t hint {};                  // Thunk.wHint
        std::uint32_t firstThunkRva {};         // Thunk.rvaFirstThunk
        std::uint32_t originalFirstThunkRva {}; // Thunk.rvaOriginalFirstThunk
        std::string   function;                 // uszFunction, may be empty
        std::string   module;                   // uszModule, may be empty
    };

    struct ImportListData
    {
        std::vector<ImportDescriptor> imports;
    };

    struct ImportListEnd
    {
        std::uint8_t  status {};
        std::uint64_t importCount {};
        std::uint8_t  truncated {};
        std::string   error;
    };

    struct UnloadedModuleListRequest
    {
        std::uint32_t pid {};
        std::string   filter;
    };

    struct UnloadedModuleDescriptor
    {
        std::uint64_t vaBase {};
        std::uint32_t imageSize {};
        bool          isWow64 {};       // fWoW64
        std::uint32_t checksum {};      // dwCheckSum, user-mode only
        std::uint32_t timeDateStamp {}; // dwTimeDateStamp, user-mode only
        std::uint64_t unloadTime {};    // ftUnload, kernel-mode only
        std::string   text;             // uszText, may be empty
    };

    struct UnloadedModuleListData
    {
        std::vector<UnloadedModuleDescriptor> modules;
    };

    struct UnloadedModuleListEnd
    {
        std::uint8_t  status {};
        std::uint64_t moduleCount {};
        std::uint8_t  truncated {};
        std::string   error;
    };

    struct ServiceListRequest
    {
        std::string filter; // service name, display name, image path, registry path, user type or account
    };

    struct ServiceDescriptor
    {
        std::uint64_t objectAddress {};           // vaObj
        std::uint32_t ordinal {};                 // dwOrdinal
        std::uint32_t startType {};               // dwStartType, 0 boot .. 4 disabled
        std::uint32_t serviceType {};             // ServiceStatus.dwServiceType
        std::uint32_t state {};                   // ServiceStatus.dwCurrentState
        std::uint32_t controlsAccepted {};        // ServiceStatus.dwControlsAccepted
        std::uint32_t win32ExitCode {};           // ServiceStatus.dwWin32ExitCode
        std::uint32_t serviceSpecificExitCode {}; // ServiceStatus.dwServiceSpecificExitCode
        std::uint32_t checkPoint {};              // ServiceStatus.dwCheckPoint
        std::uint32_t waitHint {};                // ServiceStatus.dwWaitHint
        std::uint32_t pid {};                     // dwPID, 0 for a kernel driver
        std::string   name;                       // uszServiceName, may be empty
        std::string   displayName;                // uszDisplayName, may be empty
        std::string   imagePath;                  // uszImagePath, may be empty
        std::string   path;                       // uszPath, registry key, may be empty
        std::string   userType;                   // uszUserTp, may be empty
        std::string   account;                    // uszUserAcct, may be empty
    };

    struct ServiceListData
    {
        std::vector<ServiceDescriptor> services;
    };

    struct ServiceListEnd
    {
        std::uint8_t  status {};
        std::uint64_t serviceCount {};
        std::uint8_t  truncated {};
        std::string   error;
    };

    struct UserListRequest
    {
        std::string filter; // account name or SID
    };

    struct UserDescriptor
    {
        std::string   name;       // uszText, may be empty
        std::uint64_t regHive {}; // vaRegHive
        std::string   sid;        // uszSID, may be empty
    };

    struct UserListData
    {
        std::vector<UserDescriptor> users;
    };

    struct UserListEnd
    {
        std::uint8_t  status {};
        std::uint64_t userCount {};
        std::uint8_t  truncated {};
        std::string   error;
    };

    struct NetworkListRequest
    {
        // A pid of 0 selects every process; the raw pbAddr bytes are deliberately not exposed because the header
        // does not state their byte order, so the rendered endpoint text is carried instead.
        std::uint32_t pid {};
        std::string   filter; // process name, local endpoint text or remote endpoint text
    };

    struct NetworkEndpoint
    {
        bool        valid {}; // fValid
        std::string address;  // rendered Src/Dst address followed by the port, may be empty
    };

    struct NetworkEntryDescriptor
    {
        std::uint32_t   pid {};   // dwPID
        std::string     process;  // name resolved from dwPID, may be empty
        std::uint32_t   state {}; // dwState, MemProcFS's own 0-based value: 0 closed, 1 listening, .. 4 established
        std::uint16_t   addressFamily {}; // AF
        NetworkEndpoint local;            // Src
        NetworkEndpoint remote;           // Dst
        std::uint64_t   objectAddress {}; // vaObj
        std::uint64_t   createdTime {};   // ftTime, FILETIME
        std::uint32_t   poolTag {};       // dwPoolTag
    };

    struct NetworkListData
    {
        std::vector<NetworkEntryDescriptor> entries;
    };

    struct NetworkListEnd
    {
        std::uint8_t  status {};
        std::uint64_t entryCount {};
        std::uint8_t  truncated {};
        std::string   error;
    };

    struct HeapListRequest
    {
        std::uint32_t pid {};
    };

    struct HeapDescriptor
    {
        std::uint64_t address {}; // va, the heap control-structure address
        std::uint32_t index {};   // iHeap
        std::uint32_t number {};  // dwHeapNum, the value heap-alloc selects with
        std::uint8_t  type {};    // VMMDLL_HEAP_TP: 0 n/a, 1 NT, 2 segment
        std::uint8_t  is32Bit {}; // f32
    };

    struct HeapListData
    {
        std::vector<HeapDescriptor> heaps;
    };

    struct HeapListEnd
    {
        std::uint8_t  status {};
        std::uint64_t heapCount {};
        std::uint8_t  truncated {};
        std::string   error;
    };

    struct HeapAllocListRequest
    {
        std::uint32_t pid {};
        // MemProcFS accepts either the heap number dwHeapNum or the heap address, so a single selector covers both.
        std::uint64_t heap {};
    };

    struct HeapAllocDescriptor
    {
        std::uint64_t address {}; // va
        std::uint32_t size {};    // cb
        std::uint8_t  type {};    // VMMDLL_HEAPALLOC_TP: 0 n/a, 1..4 NT, 5..8 segment
    };

    struct HeapAllocListData
    {
        std::vector<HeapAllocDescriptor> allocations;
    };

    struct HeapAllocListEnd
    {
        std::uint8_t  status {};
        std::uint64_t allocationCount {};
        std::uint8_t  truncated {};
        std::string   error;
    };

    struct KDriverListRequest
    {
        std::string filter; // name, path or service key name
    };

    struct KernelDriverDescriptor
    {
        std::uint64_t              va {};             // va
        std::uint64_t              vaDriverStart {};  // vaDriverStart
        std::uint64_t              cbDriverSize {};   // cbDriverSize
        std::uint64_t              vaDeviceObject {}; // vaDeviceObject
        std::vector<std::uint64_t> majorFunctions;    // MajorFunction[28], trailing zeros trimmed
        std::string                name;              // uszName, may be empty
        std::string                path;              // uszPath, may be empty
        std::string                serviceKeyName;    // uszServiceKeyName, may be empty
    };

    struct KDriverListData
    {
        std::vector<KernelDriverDescriptor> drivers;
    };

    struct KDriverListEnd
    {
        std::uint8_t  status {};
        std::uint64_t driverCount {};
        std::uint8_t  truncated {};
        std::string   error;
    };

    struct KDeviceListRequest
    {
        std::string filter; // device type name or volume info
    };

    struct KernelDeviceDescriptor
    {
        std::uint64_t va {};                 // va
        std::uint32_t depth {};              // iDepth
        std::uint32_t deviceType {};         // dwDeviceType, FILE_DEVICE_*
        std::uint64_t vaDriverObject {};     // vaDriverObject
        std::uint64_t vaAttachedDevice {};   // vaAttachedDevice
        std::uint64_t vaFileSystemDevice {}; // vaFileSystemDevice
        std::string   deviceTypeName;        // uszDeviceType, may be empty
        std::string   volumeInfo;            // uszVolumeInfo, may be empty
    };

    struct KDeviceListData
    {
        std::vector<KernelDeviceDescriptor> devices;
    };

    struct KDeviceListEnd
    {
        std::uint8_t  status {};
        std::uint64_t deviceCount {};
        std::uint8_t  truncated {};
        std::string   error;
    };

    struct KObjectListRequest
    {
        std::string filter; // object name or object type
    };

    struct KernelObjectDescriptor
    {
        std::uint64_t              va {};                // va
        std::uint64_t              vaParent {};          // vaParent
        std::uint64_t              childCount {};        // cvaChild, the true total
        std::vector<std::uint64_t> children;             // pvaChild, at most kMaxKernelObjectChildren
        bool                       childrenTruncated {}; // children.size() < childCount
        std::string                name;                 // uszName, may be empty
        std::string                type;                 // uszType, may be empty
    };

    struct KObjectListData
    {
        std::vector<KernelObjectDescriptor> objects;
    };

    struct KObjectListEnd
    {
        std::uint8_t  status {};
        std::uint64_t objectCount {};
        std::uint8_t  truncated {};
        std::string   error;
    };

    struct PteListRequest
    {
        std::uint32_t pid {};             // 0 = the kernel's own address space
        std::uint8_t  identifyModules {}; // fIdentifyModules; slow, and only then is uszText filled
        std::uint64_t minAddress {};      // 0 = unbounded, inclusive
        std::uint64_t maxAddress {};      // 0 = unbounded, exclusive
        std::string   filter;             // case-insensitive substring of the identified text
    };

    struct PteDescriptor
    {
        std::uint64_t vaBase {};        // vaBase
        std::uint64_t pages {};         // cPages
        std::uint64_t pageFlags {};     // fPage, VMMDLL_MEMMAP_FLAG_PAGE_*
        std::uint32_t softwarePages {}; // cSoftware, the non-active PTEs in the region
        std::uint8_t  wow64 {};         // fWoW64
        std::string   text;             // uszText, only filled when identifyModules was set
    };

    struct PteListData
    {
        std::vector<PteDescriptor> entries;
    };

    struct PteListEnd
    {
        std::uint8_t  status {};
        std::uint64_t entryCount {};
        std::uint64_t mapEntries {}; // cMap MemProcFS reported, before filtering
        std::uint8_t  truncated {};
        std::string   error;
    };

    struct PoolListRequest
    {
        std::uint8_t  big {};        // VMMDLL_POOLMAP_FLAG_BIG instead of _ALL
        std::string   tagFilter;     // case-insensitive prefix of the four-character tag
        std::uint32_t minBytes {};   // entry size floor
        std::uint64_t minAddress {}; // 0 = unbounded, inclusive
        std::uint64_t maxAddress {}; // 0 = unbounded, exclusive
    };

    struct PoolDescriptor
    {
        std::uint64_t va {};         // va
        std::uint32_t bytes {};      // cb
        std::uint32_t tag {};        // dwTag, the raw bytes; the client renders them
        std::uint8_t  alloc {};      // fAlloc
        std::uint8_t  poolType {};   // tpPool, VMMDLL_MAP_POOL_TYPE
        std::uint8_t  subsegment {}; // tpPoolSubsegment, VMM_MAP_POOL_TYPE_SUBSEGMENT
    };

    struct PoolListData
    {
        std::vector<PoolDescriptor> entries;
    };

    struct PoolListEnd
    {
        std::uint8_t  status {};
        std::uint64_t entryCount {};
        std::uint64_t mapEntries {}; // cMap MemProcFS reported, before filtering
        std::uint64_t mapBytes {};   // cbTotal, the whole map build's byte cost
        std::uint8_t  truncated {};
        std::string   error;
    };

    struct PoolTagListRequest
    {
        std::uint8_t big {};    // VMMDLL_POOLMAP_FLAG_BIG instead of _ALL
        std::string  tagFilter; // case-insensitive prefix of the four-character tag
    };

    struct PoolTagDescriptor
    {
        std::uint32_t tag {};     // szTag, the raw bytes; the client renders them
        std::uint32_t entries {}; // cEntry, the number of allocations carrying the tag
    };

    struct PoolTagListData
    {
        std::vector<PoolTagDescriptor> tags;
    };

    struct PoolTagListEnd
    {
        std::uint8_t  status {};
        std::uint64_t entryCount {}; // delivered tag count
        std::uint64_t mapTags {};    // cTag MemProcFS reported, before filtering
        std::uint64_t mapEntries {}; // cMap MemProcFS reported
        std::uint8_t  truncated {};
        std::string   error;
    };

    // The allow-listed VMMDLL_ConfigSet options. The wire values are stable and independent of the vendored
    // MemProcFS constants; the host maps them. Only options the project has decided to expose are present.
    enum class ConfigOption : std::uint8_t
    {
        TickPeriod = 0,
        ReadCacheTicks,
        TlbCacheTicks,
        ProcCacheTicksPartial,
        ProcCacheTicksTotal,
        RefreshAll,
        RefreshFreqMem,
        RefreshFreqMemPartial,
        RefreshFreqTlb,
        RefreshFreqTlbPartial,
        RefreshFreqFast,
        RefreshFreqMedium,
        RefreshFreqSlow,
        RefreshSpecificHeapAlloc,
        RefreshSpecificKobject,
        RefreshSpecificNet,
        RefreshSpecificPfn,
        RefreshSpecificPhysmemmap,
        RefreshSpecificPool,
        RefreshSpecificRegistry,
        RefreshSpecificServices,
        RefreshSpecificThreadcs,
        RefreshSpecificUser,
        RefreshSpecificVm,
        RefreshSpecificProcess,
        ProcessDtb,
        ProcessDtbFastLowIntegrity,
    };

    [[nodiscard]] std::optional<ConfigOption> parseConfigOption(std::string_view name) noexcept;
    [[nodiscard]] std::string_view            configOptionName(ConfigOption option) noexcept;
    [[nodiscard]] bool                        configOptionUsesPid(ConfigOption option) noexcept;
    [[nodiscard]] bool                        configOptionRequiresValue(ConfigOption option) noexcept;
    [[nodiscard]] bool                        isValidConfigOption(std::uint8_t value) noexcept;

    struct ConfigSetRequest
    {
        ConfigOption  option {ConfigOption::TickPeriod};
        std::uint32_t pid {}; // process-scoped options only; elsewhere 0
        std::uint64_t value {};
    };

    struct ConfigSetResponse
    {
        std::uint64_t value {}; // the value the host applied
    };

    struct PfnListRequest
    {
        std::uint32_t pfnStart {};
        std::uint32_t pfnCount {}; // 1..kMaxPfnEntries
    };

    struct PfnDescriptor
    {
        std::uint32_t pfn {};
        std::uint8_t  typeExtended {};   // tpExtended, VMMDLL_MAP_PFN_TYPEEXTENDED
        std::uint8_t  pageLocation {};   // PageLocation, VMMDLL_MAP_PFN_TYPE
        std::uint32_t pid {};            // AddressInfo.dwPid, only on an active non-prototype page
        std::uint64_t va {};             // AddressInfo.va
        std::uint64_t vaPte {};          // vaPte
        std::uint64_t originalPte {};    // OriginalPte
        std::uint32_t pteFrame {};       // PteFrame
        std::uint16_t referenceCount {}; // ReferenceCount
        std::uint8_t  cacheAttribute {}; // CacheAttribute
        std::uint8_t  priority {};       // Priority
        std::uint8_t  flags {};          // the MMPFNENTRY booleans packed into one byte
        std::uint8_t  pageColor {};      // PageColor
    };

    struct PfnListData
    {
        std::vector<PfnDescriptor> entries;
    };

    struct PfnListEnd
    {
        std::uint8_t  status {};
        std::uint64_t entryCount {};
        std::string   error;
    };

    [[nodiscard]] std::vector<std::byte>           encodeHandleListRequest(const HandleListRequest& message);
    [[nodiscard]] std::optional<HandleListRequest> decodeHandleListRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>        encodeHandleListData(const HandleListData& message);
    [[nodiscard]] std::optional<HandleListData> decodeHandleListData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>       encodeHandleListEnd(const HandleListEnd& message);
    [[nodiscard]] std::optional<HandleListEnd> decodeHandleListEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>           encodeExportListRequest(const ExportListRequest& message);
    [[nodiscard]] std::optional<ExportListRequest> decodeExportListRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>        encodeExportListData(const ExportListData& message);
    [[nodiscard]] std::optional<ExportListData> decodeExportListData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>       encodeExportListEnd(const ExportListEnd& message);
    [[nodiscard]] std::optional<ExportListEnd> decodeExportListEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>           encodeImportListRequest(const ImportListRequest& message);
    [[nodiscard]] std::optional<ImportListRequest> decodeImportListRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>        encodeImportListData(const ImportListData& message);
    [[nodiscard]] std::optional<ImportListData> decodeImportListData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>       encodeImportListEnd(const ImportListEnd& message);
    [[nodiscard]] std::optional<ImportListEnd> decodeImportListEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte> encodeUnloadedModuleListRequest(const UnloadedModuleListRequest& message);
    [[nodiscard]] std::optional<UnloadedModuleListRequest>
    decodeUnloadedModuleListRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte> encodeUnloadedModuleListData(const UnloadedModuleListData& message);
    [[nodiscard]] std::optional<UnloadedModuleListData>
    decodeUnloadedModuleListData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte> encodeUnloadedModuleListEnd(const UnloadedModuleListEnd& message);
    [[nodiscard]] std::optional<UnloadedModuleListEnd> decodeUnloadedModuleListEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>            encodeServiceListRequest(const ServiceListRequest& message);
    [[nodiscard]] std::optional<ServiceListRequest> decodeServiceListRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>         encodeServiceListData(const ServiceListData& message);
    [[nodiscard]] std::optional<ServiceListData> decodeServiceListData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>        encodeServiceListEnd(const ServiceListEnd& message);
    [[nodiscard]] std::optional<ServiceListEnd> decodeServiceListEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>         encodeUserListRequest(const UserListRequest& message);
    [[nodiscard]] std::optional<UserListRequest> decodeUserListRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>      encodeUserListData(const UserListData& message);
    [[nodiscard]] std::optional<UserListData> decodeUserListData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>     encodeUserListEnd(const UserListEnd& message);
    [[nodiscard]] std::optional<UserListEnd> decodeUserListEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>            encodeNetworkListRequest(const NetworkListRequest& message);
    [[nodiscard]] std::optional<NetworkListRequest> decodeNetworkListRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>         encodeNetworkListData(const NetworkListData& message);
    [[nodiscard]] std::optional<NetworkListData> decodeNetworkListData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>        encodeNetworkListEnd(const NetworkListEnd& message);
    [[nodiscard]] std::optional<NetworkListEnd> decodeNetworkListEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>         encodeHeapListRequest(const HeapListRequest& message);
    [[nodiscard]] std::optional<HeapListRequest> decodeHeapListRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>      encodeHeapListData(const HeapListData& message);
    [[nodiscard]] std::optional<HeapListData> decodeHeapListData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>     encodeHeapListEnd(const HeapListEnd& message);
    [[nodiscard]] std::optional<HeapListEnd> decodeHeapListEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>              encodeHeapAllocListRequest(const HeapAllocListRequest& message);
    [[nodiscard]] std::optional<HeapAllocListRequest> decodeHeapAllocListRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>           encodeHeapAllocListData(const HeapAllocListData& message);
    [[nodiscard]] std::optional<HeapAllocListData> decodeHeapAllocListData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>          encodeHeapAllocListEnd(const HeapAllocListEnd& message);
    [[nodiscard]] std::optional<HeapAllocListEnd> decodeHeapAllocListEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>            encodeKDriverListRequest(const KDriverListRequest& message);
    [[nodiscard]] std::optional<KDriverListRequest> decodeKDriverListRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>         encodeKDriverListData(const KDriverListData& message);
    [[nodiscard]] std::optional<KDriverListData> decodeKDriverListData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>        encodeKDriverListEnd(const KDriverListEnd& message);
    [[nodiscard]] std::optional<KDriverListEnd> decodeKDriverListEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>            encodeKDeviceListRequest(const KDeviceListRequest& message);
    [[nodiscard]] std::optional<KDeviceListRequest> decodeKDeviceListRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>         encodeKDeviceListData(const KDeviceListData& message);
    [[nodiscard]] std::optional<KDeviceListData> decodeKDeviceListData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>        encodeKDeviceListEnd(const KDeviceListEnd& message);
    [[nodiscard]] std::optional<KDeviceListEnd> decodeKDeviceListEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>            encodeKObjectListRequest(const KObjectListRequest& message);
    [[nodiscard]] std::optional<KObjectListRequest> decodeKObjectListRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>         encodeKObjectListData(const KObjectListData& message);
    [[nodiscard]] std::optional<KObjectListData> decodeKObjectListData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>        encodeKObjectListEnd(const KObjectListEnd& message);
    [[nodiscard]] std::optional<KObjectListEnd> decodeKObjectListEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>        encodePteListRequest(const PteListRequest& message);
    [[nodiscard]] std::optional<PteListRequest> decodePteListRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>     encodePteListData(const PteListData& message);
    [[nodiscard]] std::optional<PteListData> decodePteListData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>    encodePteListEnd(const PteListEnd& message);
    [[nodiscard]] std::optional<PteListEnd> decodePteListEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>         encodePoolListRequest(const PoolListRequest& message);
    [[nodiscard]] std::optional<PoolListRequest> decodePoolListRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>      encodePoolListData(const PoolListData& message);
    [[nodiscard]] std::optional<PoolListData> decodePoolListData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>     encodePoolListEnd(const PoolListEnd& message);
    [[nodiscard]] std::optional<PoolListEnd> decodePoolListEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>            encodePoolTagListRequest(const PoolTagListRequest& message);
    [[nodiscard]] std::optional<PoolTagListRequest> decodePoolTagListRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>         encodePoolTagListData(const PoolTagListData& message);
    [[nodiscard]] std::optional<PoolTagListData> decodePoolTagListData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>        encodePoolTagListEnd(const PoolTagListEnd& message);
    [[nodiscard]] std::optional<PoolTagListEnd> decodePoolTagListEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>        encodePfnListRequest(const PfnListRequest& message);
    [[nodiscard]] std::optional<PfnListRequest> decodePfnListRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>     encodePfnListData(const PfnListData& message);
    [[nodiscard]] std::optional<PfnListData> decodePfnListData(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>    encodePfnListEnd(const PfnListEnd& message);
    [[nodiscard]] std::optional<PfnListEnd> decodePfnListEnd(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>          encodeConfigSetRequest(const ConfigSetRequest& message);
    [[nodiscard]] std::optional<ConfigSetRequest> decodeConfigSetRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>           encodeConfigSetResponse(const ConfigSetResponse& message);
    [[nodiscard]] std::optional<ConfigSetResponse> decodeConfigSetResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>             encodeDebugControlRequest(const DebugControlRequest& message);
    [[nodiscard]] std::optional<DebugControlRequest> decodeDebugControlRequest(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>            encodeDebugStateResponse(const DebugStateResponse& message);
    [[nodiscard]] std::optional<DebugStateResponse> decodeDebugStateResponse(std::span<const std::byte> payload);

    [[nodiscard]] std::vector<std::byte>              encodeDebugControlResponse(const DebugControlResponse& message);
    [[nodiscard]] std::optional<DebugControlResponse> decodeDebugControlResponse(std::span<const std::byte> payload);
} // namespace mpfshost

#endif // MPFSHOST_CORE_IPC_MESSAGE_HPP
