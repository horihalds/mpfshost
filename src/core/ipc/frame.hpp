#ifndef MPFSHOST_CORE_IPC_FRAME_HPP
#define MPFSHOST_CORE_IPC_FRAME_HPP
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mpfshost
{
    inline constexpr std::uint32_t kProtocolMagic    = 0x5350464D; // 'M','F','P','S'
    inline constexpr std::uint16_t kProtocolVersion  = 1;
    inline constexpr std::uint32_t kMaxPayloadLength = 1u << 20;
    inline constexpr std::size_t   kFrameHeaderSize  = 20;

    inline constexpr std::uint32_t kMaxReadSize                 = 64u << 20;
    inline constexpr std::size_t   kMaxScatterHandlesPerSession = 8;
    inline constexpr std::size_t   kMaxScatterEntries           = 4096;
    inline constexpr std::uint64_t kMaxScatterReadBytes         = 64ull << 20;

    inline constexpr std::uint32_t kMaxMapBatchBytes   = 512u << 10;
    inline constexpr std::size_t   kMaxMapStringLength = 1024;

    // Handle, module-table, unloaded-module and system maps grow with the target, not with the request, so each
    // area is capped and the end frame reports whether a cap cut the answer short.
    inline constexpr std::size_t kMaxHandleEntries         = 0x10000;
    inline constexpr std::size_t kMaxModuleTableEntries    = 0x10000;
    inline constexpr std::size_t kMaxUnloadedModuleEntries = 0x1000;
    inline constexpr std::size_t kMaxServiceEntries        = 0x1000;
    inline constexpr std::size_t kMaxUserEntries           = 0x400;
    inline constexpr std::size_t kMaxNetworkEntries        = 0x10000;
    inline constexpr std::size_t kMaxHeapEntries           = 0x100;
    inline constexpr std::size_t kMaxHeapAllocEntries      = 0x40000;
    inline constexpr std::size_t kMaxKernelDriverEntries   = 0x400;
    inline constexpr std::size_t kMaxKernelDeviceEntries   = 0x1000;
    inline constexpr std::size_t kMaxKernelObjectEntries   = 0x8000;
    inline constexpr std::size_t kMaxKernelObjectChildren  = 0x40;
    inline constexpr std::size_t kMaxPteEntries            = 0x20000;
    inline constexpr std::size_t kMaxPoolEntries           = 0x20000;
    inline constexpr std::size_t kMaxPfnEntries            = 0x8000;

    // Page reads and prefetches expand into page-aligned addresses in unprivileged code, so the caps are
    // enforced before any host call can allocate.
    inline constexpr std::uint64_t kPageSize         = 0x1000;
    inline constexpr std::size_t   kMaxPageCount     = 4096;    // 16 MiB of page dumps per request
    inline constexpr std::size_t   kMaxPrefetchPages = 0x10000; // 256 MiB of cache warming per request

    // MemProcFS caps a search pattern at 32 bytes, a search result at 0x10000 hits and interprets a search
    // alignment as a power-of-two byte count.
    inline constexpr std::size_t   kMaxSearchPatternLength = 32;
    inline constexpr std::size_t   kMaxSearchHits          = 0x10000;
    inline constexpr std::uint32_t kMaxSearchAlignBytes    = 4096;

    // MemProcFS addresses the bound target's own physical memory as dwPID = (DWORD)-1. The page, prefetch
    // and search requests accept this pid in place of a process id.
    inline constexpr std::uint32_t kPhysicalPid = 0xFFFFFFFFu;

    // MEM_SCATTER rules: an entry never exceeds a page and never crosses a page boundary, and the vendored
    // MemProcFS build rejects anything shorter than eight bytes.
    inline constexpr std::uint32_t kPhysicalScatterPageSize   = 0x1000;
    inline constexpr std::uint32_t kPhysicalScatterMinBytes   = 8;
    inline constexpr std::size_t   kMaxPhysicalScatterEntries = kMaxScatterEntries;
    inline constexpr std::uint64_t kMaxPhysicalScatterBytes   = kMaxScatterReadBytes;

    // Request/response framing is fixed per type. A single-frame request is answered by exactly one frame of
    // its response type (or one ErrorResponse). A streamed request is answered by zero or more data frames
    // followed by exactly one end frame, all carrying the request id, or by one ErrorResponse instead.
    enum class MessageType : std::uint16_t
    {
        ListTargetsRequest           = 0x0001,
        BindRequest                  = 0x0002,
        ProcessListRequest           = 0x0003,
        MemoryReadRequest            = 0x0004, // streamed: MemoryReadData* then MemoryReadEnd
        MemoryWriteRequest           = 0x0005,
        ScatterCreateRequest         = 0x0006,
        ScatterPrepareRequest        = 0x0007,
        ScatterExecuteRequest        = 0x0008, // streamed: ScatterExecuteData* then ScatterExecuteEnd
        ScatterCloseRequest          = 0x0009,
        ModuleListRequest            = 0x000A, // streamed: ModuleListData* then ModuleListEnd
        MemoryMapRequest             = 0x000B, // streamed: MemoryMapData* then MemoryMapEnd
        ThreadListRequest            = 0x000C, // streamed: ThreadListData* then ThreadListEnd
        VmListRequest                = 0x000D,
        PhysicalReadRequest          = 0x000E, // streamed: PhysicalReadData* then PhysicalReadEnd
        PhysicalWriteRequest         = 0x000F,
        GpaTranslateRequest          = 0x0010,
        Virt2PhysRequest             = 0x0011,
        PhysicalScatterRequest       = 0x0012, // streamed: PhysicalScatterData* then PhysicalScatterEnd
        VfsListRequest               = 0x0013, // streamed: VfsListData* then VfsListEnd
        VfsReadRequest               = 0x0014, // streamed: VfsReadData* then VfsReadEnd
        ProcessInfoRequest           = 0x0015,
        SymbolLookupRequest          = 0x0016,
        ModuleLayoutRequest          = 0x0017,
        CancelRequest                = 0x0018,
        VirtualQueryRequest          = 0x0019,
        MemorySearchRequest          = 0x001A, // streamed: MemorySearchData* then MemorySearchEnd
        PageReadRequest              = 0x001B, // streamed: PageReadData* then PageReadEnd
        PrefetchRequest              = 0x001C,
        HandleListRequest            = 0x001D, // streamed: HandleListData* then HandleListEnd
        ExportListRequest            = 0x001E, // streamed: ExportListData* then ExportListEnd
        ImportListRequest            = 0x001F, // streamed: ImportListData* then ImportListEnd
        UnloadedModuleListRequest    = 0x0020, // streamed: UnloadedModuleListData* then UnloadedModuleListEnd
        ServiceListRequest           = 0x0021, // streamed: ServiceListData* then ServiceListEnd
        UserListRequest              = 0x0022, // streamed: UserListData* then UserListEnd
        NetworkListRequest           = 0x0023, // streamed: NetworkListData* then NetworkListEnd
        HeapListRequest              = 0x0024, // streamed: HeapListData* then HeapListEnd
        HeapAllocListRequest         = 0x0025, // streamed: HeapAllocListData* then HeapAllocListEnd
        KDriverListRequest           = 0x0026, // streamed: KDriverListData* then KDriverListEnd
        KDeviceListRequest           = 0x0027, // streamed: KDeviceListData* then KDeviceListEnd
        KObjectListRequest           = 0x0028, // streamed: KObjectListData* then KObjectListEnd
        PteListRequest               = 0x0029, // streamed: PteListData* then PteListEnd
        PoolListRequest              = 0x002A, // streamed: PoolListData* then PoolListEnd
        PfnListRequest               = 0x002B, // streamed: PfnListData* then PfnListEnd
        CapabilitiesRequest          = 0x002C,
        PdbLoadRequest               = 0x002D,
        PdbSymbolNameRequest         = 0x002E,
        PdbSymbolAddressRequest      = 0x002F,
        PdbTypeSizeRequest           = 0x0030,
        PdbTypeChildOffsetRequest    = 0x0031,
        ProcessListAllRequest        = 0x0032,
        VadExListRequest             = 0x0033, // streamed: VadExListData* then VadExListEnd
        ScatterExecuteReadRequest    = 0x0034, // streamed: ScatterExecuteReadData* then ScatterExecuteReadEnd
        ScatterClearRequest          = 0x0035,
        PoolTagListRequest           = 0x0036, // streamed: PoolTagListData* then PoolTagListEnd
        ConfigSetRequest             = 0x0037,
        VfsWriteRequest              = 0x0038,
        ModuleByAddressRequest       = 0x0039,
        ProcessByDtbRequest          = 0x003A,
        DebugStateRequest            = 0x003B,
        DebugControlRequest          = 0x003C,
        DebugRegistersRequest        = 0x003D,
        DebugWatchRequest            = 0x003E, // streamed: DebugWatchData* then DebugWatchEnd
        DebugBreakpointRequest       = 0x003F,
        DebugStepRequest             = 0x0040,
        DebugDetachRequest           = 0x0041,
        DebugAttachRequest           = 0x0042,
        DebugBreakpointArmRequest    = 0x0043,
        DebugBreakpointRemoveRequest = 0x0044,
        DebugBreakpointListRequest   = 0x0045,
        DebugWatchArmRequest         = 0x0046,
        DebugWatchRemoveRequest      = 0x0047,
        DebugWatchListRequest        = 0x0048,
        DebugContinueRequest         = 0x0049,
        DebugWaitRequest             = 0x004A, // streamed: DebugWaitHit* then DebugWaitEnd
        DebugThreadsRequest          = 0x004B,
        ListTargetsResponse          = 0x8001,
        BindResponse                 = 0x8002,
        ProcessListResponse          = 0x8003,
        MemoryReadData               = 0x8004,
        MemoryReadEnd                = 0x8005,
        MemoryWriteResponse          = 0x8006,
        ScatterCreateResponse        = 0x8007,
        ScatterPrepareResponse       = 0x8008,
        ScatterExecuteData           = 0x8009,
        ScatterExecuteEnd            = 0x800A,
        ScatterCloseResponse         = 0x800B,
        ModuleListData               = 0x800C,
        ModuleListEnd                = 0x800D,
        MemoryMapData                = 0x800E,
        MemoryMapEnd                 = 0x800F,
        ThreadListData               = 0x8010,
        ThreadListEnd                = 0x8011,
        VmListResponse               = 0x8012,
        PhysicalReadData             = 0x8013,
        PhysicalReadEnd              = 0x8014,
        PhysicalWriteResponse        = 0x8015,
        GpaTranslateResponse         = 0x8016,
        Virt2PhysResponse            = 0x8017,
        PhysicalScatterData          = 0x8018,
        PhysicalScatterEnd           = 0x8019,
        VfsListData                  = 0x801A,
        VfsListEnd                   = 0x801B,
        VfsReadData                  = 0x801C,
        VfsReadEnd                   = 0x801D,
        ProcessInfoResponse          = 0x801E,
        SymbolLookupResponse         = 0x801F,
        ModuleLayoutResponse         = 0x8020,
        CancelResponse               = 0x8021,
        VirtualQueryResponse         = 0x8022,
        MemorySearchData             = 0x8023,
        MemorySearchEnd              = 0x8024,
        PageReadData                 = 0x8025,
        PageReadEnd                  = 0x8026,
        PrefetchResponse             = 0x8027,
        HandleListData               = 0x8028,
        HandleListEnd                = 0x8029,
        ExportListData               = 0x802A,
        ExportListEnd                = 0x802B,
        ImportListData               = 0x802C,
        ImportListEnd                = 0x802D,
        UnloadedModuleListData       = 0x802E,
        UnloadedModuleListEnd        = 0x802F,
        ServiceListData              = 0x8036,
        ServiceListEnd               = 0x8037,
        UserListData                 = 0x8038,
        UserListEnd                  = 0x8039,
        NetworkListData              = 0x803A,
        NetworkListEnd               = 0x803B,
        HeapListData                 = 0x803C,
        HeapListEnd                  = 0x803D,
        HeapAllocListData            = 0x803E,
        HeapAllocListEnd             = 0x803F,
        KDriverListData              = 0x8040,
        KDriverListEnd               = 0x8041,
        KDeviceListData              = 0x8042,
        KDeviceListEnd               = 0x8043,
        KObjectListData              = 0x8044,
        KObjectListEnd               = 0x8045,
        PteListData                  = 0x8046,
        PteListEnd                   = 0x8047,
        PoolListData                 = 0x8048,
        PoolListEnd                  = 0x8049,
        PfnListData                  = 0x804A,
        PfnListEnd                   = 0x804B,
        CapabilitiesResponse         = 0x804C,
        PdbLoadResponse              = 0x804D,
        PdbSymbolNameResponse        = 0x804E,
        PdbSymbolAddressResponse     = 0x804F,
        PdbTypeSizeResponse          = 0x8050,
        PdbTypeChildOffsetResponse   = 0x8051,
        ProcessListAllResponse       = 0x8052,
        VadExListData                = 0x8053,
        VadExListEnd                 = 0x8054,
        ScatterExecuteReadData       = 0x8055,
        ScatterExecuteReadEnd        = 0x8056,
        ScatterClearResponse         = 0x8057,
        PoolTagListData              = 0x8058,
        PoolTagListEnd               = 0x8059,
        ConfigSetResponse            = 0x805A,
        VfsWriteResponse             = 0x805B,
        ModuleByAddressResponse      = 0x805C,
        ProcessByDtbResponse         = 0x805D,
        DebugStateResponse           = 0x805E,
        DebugControlResponse         = 0x805F,
        DebugRegistersResponse       = 0x8060,
        DebugWatchData               = 0x8061,
        DebugWatchEnd                = 0x8062,
        DebugBreakpointResponse      = 0x8063,
        DebugStepResponse            = 0x8064,
        DebugDetachResponse          = 0x8065,
        DebugAttachResponse          = 0x8066,
        DebugBreakpointArmResponse   = 0x8067,
        DebugResourceRemoveResponse  = 0x8068,
        DebugBreakpointListResponse  = 0x8069,
        DebugWatchArmResponse        = 0x806A,
        DebugWatchListResponse       = 0x806B,
        DebugContinueResponse        = 0x806C,
        DebugWaitHit                 = 0x806D,
        DebugWaitEnd                 = 0x806E,
        DebugThreadsResponse         = 0x806F,
        ErrorResponse                = 0x8FFF,
    };

    enum class ErrorCode : std::uint16_t
    {
        MalformedPayload      = 1,
        UnsupportedVersion    = 2,
        UnknownMessage        = 3,
        TargetNotFound        = 4,
        TargetUnavailable     = 5,
        SessionAlreadyBound   = 6,
        Internal              = 7,
        NotBound              = 8,
        CapabilityUnavailable = 9,
        ResultTooLarge        = 10,
        UnknownScatter        = 11,
        InvalidRange          = 12,
        ScatterLimitReached   = 13,
        UnknownVm             = 14,
        NotFound              = 15,
        AccessDenied          = 16,
        Cancelled             = 17,
        Timeout               = 18,
        Busy                  = 19,
        InvalidState          = 20,
    };

    [[nodiscard]] std::string_view messageTypeName(MessageType type) noexcept;
    [[nodiscard]] std::string_view errorCodeName(ErrorCode code) noexcept;

    struct FrameHeader
    {
        std::uint16_t version {};
        MessageType   type {};
        std::uint64_t requestId {};
        std::uint32_t payloadLength {};
    };

    enum class FrameReadStatus
    {
        Complete, // a whole, valid frame was read
        Rejected, // the header was readable but the frame is not acceptable
        Timeout,  // the read timed out; a partly read frame is not resumable
        Closed,   // the peer went away or the stream is unusable
    };

    class ByteWriter
    {
    public:
        void writeU8(std::uint8_t value);
        void writeU16(std::uint16_t value);
        void writeU32(std::uint32_t value);
        void writeU64(std::uint64_t value);
        void writeString(std::string_view value);
        void writeBytes(std::span<const std::byte> value);

        [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept;
        [[nodiscard]] std::vector<std::byte>        take();

    private:
        std::vector<std::byte> _buffer;
    };

    class ByteReader
    {
    public:
        explicit ByteReader(std::span<const std::byte> data) noexcept;

        [[nodiscard]] std::uint8_t           readU8();
        [[nodiscard]] std::uint16_t          readU16();
        [[nodiscard]] std::uint32_t          readU32();
        [[nodiscard]] std::uint64_t          readU64();
        [[nodiscard]] std::string            readString();
        [[nodiscard]] std::vector<std::byte> readBytes();

        [[nodiscard]] bool ok() const noexcept;
        [[nodiscard]] bool atEnd() const noexcept;

    private:
        [[nodiscard]] bool readBytes(std::byte* destination, std::size_t count);

        std::span<const std::byte> _data;
        std::size_t                _offset {};
        bool                       _failed {};
    };

    [[nodiscard]] std::vector<std::byte>
    encodeFrame(MessageType type, std::uint64_t requestId, std::span<const std::byte> payload);
    [[nodiscard]] FrameReadStatus
    readFrame(int fd, FrameHeader& header, std::vector<std::byte>& payload, std::string& error);
    // Header of the next frame when the whole frame is already buffered, so a caller can inspect it before
    // consuming it; nothing when the buffer holds no complete, valid frame.
    [[nodiscard]] std::optional<FrameHeader>   bufferedFrameHeader(int fd);
    // Payload length of the next frame when the whole frame is already buffered, so a caller can consume it
    // without blocking; nothing when the buffer holds no complete, valid frame.
    [[nodiscard]] std::optional<std::uint32_t> bufferedFrameLength(int fd);
    [[nodiscard]] bool                         writeFrame(
        int fd, MessageType type, std::uint64_t requestId, std::span<const std::byte> payload, std::string& error);
} // namespace mpfshost

#endif // MPFSHOST_CORE_IPC_FRAME_HPP
