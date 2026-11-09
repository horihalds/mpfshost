#include <catch2/catch.hpp>

#include "core/ipc/frame.hpp"
#include "core/ipc/message.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace
{
    class FuzzSocket
    {
    public:
        FuzzSocket()
        {
            int fds[2] {};
            REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
            _readFd  = fds[0];
            _writeFd = fds[1];
        }

        ~FuzzSocket()
        {
            if (_readFd >= 0)
            {
                ::close(_readFd);
            }
            if (_writeFd >= 0)
            {
                ::close(_writeFd);
            }
        }

        FuzzSocket(const FuzzSocket&)            = delete;
        FuzzSocket& operator=(const FuzzSocket&) = delete;

        [[nodiscard]] int readFd() const noexcept
        {
            return _readFd;
        }

        void writeAndClose(std::span<const std::byte> bytes) const
        {
            std::size_t written = 0;
            while (written < bytes.size())
            {
                const auto result = ::send(_writeFd, bytes.data() + written, bytes.size() - written, MSG_NOSIGNAL);
                REQUIRE(result > 0);
                written += static_cast<std::size_t>(result);
            }
            REQUIRE(::shutdown(_writeFd, SHUT_WR) == 0);
        }

    private:
        int _readFd {-1};
        int _writeFd {-1};
    };

    std::vector<std::byte> randomBytes(std::mt19937& rng, std::size_t length)
    {
        std::uniform_int_distribution<int> byteDistribution {0, 255};
        std::vector<std::byte>             bytes;
        bytes.reserve(length);
        for (std::size_t i = 0; i < length; ++i)
        {
            bytes.push_back(static_cast<std::byte>(byteDistribution(rng)));
        }
        return bytes;
    }

    std::vector<std::byte> rawHeader(std::uint32_t magic,
                                     std::uint16_t version,
                                     std::uint16_t type,
                                     std::uint64_t requestId,
                                     std::uint32_t payloadLength)
    {
        mpfshost::ByteWriter writer;
        writer.writeU32(magic);
        writer.writeU16(version);
        writer.writeU16(type);
        writer.writeU64(requestId);
        writer.writeU32(payloadLength);
        return writer.take();
    }

    mpfshost::FrameReadStatus readStatus(std::span<const std::byte> bytes)
    {
        FuzzSocket             socket;
        mpfshost::FrameHeader  header;
        std::vector<std::byte> payload;
        std::string            error;
        socket.writeAndClose(bytes);
        return mpfshost::readFrame(socket.readFd(), header, payload, error);
    }

    void requireRejectedOrClosed(mpfshost::FrameReadStatus status)
    {
        REQUIRE((status == mpfshost::FrameReadStatus::Rejected || status == mpfshost::FrameReadStatus::Closed));
    }

    using DecodeProbe = std::pair<const char*, std::function<bool(std::span<const std::byte>)>>;

    std::vector<DecodeProbe> decodeProbes()
    {
        return {
            {       "decodeListTargetsResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeListTargetsResponse(p).has_value();
             }},
            {               "decodeBindRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeBindRequest(p).has_value();
             }},
            {              "decodeBindResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeBindResponse(p).has_value();
             }},
            {             "decodeErrorResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeErrorResponse(p).has_value();
             }},
            {        "decodeProcessListRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeProcessListRequest(p).has_value();
             }},
            {       "decodeProcessListResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeProcessListResponse(p).has_value();
             }},
            {    "decodeProcessListAllResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeProcessListAllResponse(p).has_value();
             }},
            {         "decodeMemoryReadRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeMemoryReadRequest(p).has_value();
             }},
            {             "decodeMemoryReadEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeMemoryReadEnd(p).has_value();
             }},
            {        "decodeMemoryWriteRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeMemoryWriteRequest(p).has_value();
             }},
            {       "decodeMemoryWriteResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeMemoryWriteResponse(p).has_value();
             }},
            {      "decodeScatterCreateRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeScatterCreateRequest(p).has_value();
             }},
            {     "decodeScatterCreateResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeScatterCreateResponse(p).has_value();
             }},
            {     "decodeScatterPrepareRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeScatterPrepareRequest(p).has_value();
             }},
            {    "decodeScatterPrepareResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeScatterPrepareResponse(p).has_value();
             }},
            {     "decodeScatterExecuteRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeScatterExecuteRequest(p).has_value();
             }},
            {        "decodeScatterExecuteData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeScatterExecuteData(p).has_value();
             }},
            {         "decodeScatterExecuteEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeScatterExecuteEnd(p).has_value();
             }},
            {       "decodeScatterCloseRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeScatterCloseRequest(p).has_value();
             }},
            {      "decodeScatterCloseResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeScatterCloseResponse(p).has_value();
             }},
            { "decodeScatterExecuteReadRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeScatterExecuteReadRequest(p).has_value();
             }},
            {    "decodeScatterExecuteReadData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeScatterExecuteReadData(p).has_value();
             }},
            {     "decodeScatterExecuteReadEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeScatterExecuteReadEnd(p).has_value();
             }},
            {       "decodeScatterClearRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeScatterClearRequest(p).has_value();
             }},
            {      "decodeScatterClearResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeScatterClearResponse(p).has_value();
             }},
            {         "decodeModuleListRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeModuleListRequest(p).has_value();
             }},
            {            "decodeModuleListData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeModuleListData(p).has_value();
             }},
            {             "decodeModuleListEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeModuleListEnd(p).has_value();
             }},
            {          "decodeMemoryMapRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeMemoryMapRequest(p).has_value();
             }},
            {             "decodeMemoryMapData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeMemoryMapData(p).has_value();
             }},
            {              "decodeMemoryMapEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeMemoryMapEnd(p).has_value();
             }},
            {          "decodeVadExListRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeVadExListRequest(p).has_value();
             }},
            {             "decodeVadExListData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeVadExListData(p).has_value();
             }},
            {              "decodeVadExListEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeVadExListEnd(p).has_value();
             }},
            {         "decodeThreadListRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeThreadListRequest(p).has_value();
             }},
            {            "decodeThreadListData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeThreadListData(p).has_value();
             }},
            {             "decodeThreadListEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeThreadListEnd(p).has_value();
             }},
            {            "decodeVmListResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeVmListResponse(p).has_value();
             }},
            {       "decodePhysicalReadRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePhysicalReadRequest(p).has_value();
             }},
            {           "decodePhysicalReadEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePhysicalReadEnd(p).has_value();
             }},
            {      "decodePhysicalWriteRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePhysicalWriteRequest(p).has_value();
             }},
            {       "decodeGpaTranslateRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeGpaTranslateRequest(p).has_value();
             }},
            {      "decodeGpaTranslateResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeGpaTranslateResponse(p).has_value();
             }},
            {          "decodeVirt2PhysRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeVirt2PhysRequest(p).has_value();
             }},
            {         "decodeVirt2PhysResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeVirt2PhysResponse(p).has_value();
             }},
            {    "decodePhysicalScatterRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePhysicalScatterRequest(p).has_value();
             }},
            {            "decodeVfsListRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeVfsListRequest(p).has_value();
             }},
            {               "decodeVfsListData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeVfsListData(p).has_value();
             }},
            {                "decodeVfsListEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeVfsListEnd(p).has_value();
             }},
            {            "decodeVfsReadRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeVfsReadRequest(p).has_value();
             }},
            {                "decodeVfsReadEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeVfsReadEnd(p).has_value();
             }},
            {           "decodeVfsWriteRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeVfsWriteRequest(p).has_value();
             }},
            {          "decodeVfsWriteResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeVfsWriteResponse(p).has_value();
             }},
            {        "decodeProcessInfoRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeProcessInfoRequest(p).has_value();
             }},
            {       "decodeProcessInfoResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeProcessInfoResponse(p).has_value();
             }},
            {       "decodeProcessByDtbRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeProcessByDtbRequest(p).has_value();
             }},
            {      "decodeProcessByDtbResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeProcessByDtbResponse(p).has_value();
             }},
            {    "decodeModuleByAddressRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeModuleByAddressRequest(p).has_value();
             }},
            {   "decodeModuleByAddressResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeModuleByAddressResponse(p).has_value();
             }},
            {       "decodeSymbolLookupRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeSymbolLookupRequest(p).has_value();
             }},
            {      "decodeSymbolLookupResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeSymbolLookupResponse(p).has_value();
             }},
            {            "decodePdbLoadRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePdbLoadRequest(p).has_value();
             }},
            {           "decodePdbLoadResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePdbLoadResponse(p).has_value();
             }},
            {      "decodePdbSymbolNameRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePdbSymbolNameRequest(p).has_value();
             }},
            {     "decodePdbSymbolNameResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePdbSymbolNameResponse(p).has_value();
             }},
            {   "decodePdbSymbolAddressRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePdbSymbolAddressRequest(p).has_value();
             }},
            {  "decodePdbSymbolAddressResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePdbSymbolAddressResponse(p).has_value();
             }},
            {        "decodePdbTypeSizeRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePdbTypeSizeRequest(p).has_value();
             }},
            {       "decodePdbTypeSizeResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePdbTypeSizeResponse(p).has_value();
             }},
            { "decodePdbTypeChildOffsetRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePdbTypeChildOffsetRequest(p).has_value();
             }},
            {"decodePdbTypeChildOffsetResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePdbTypeChildOffsetResponse(p).has_value();
             }},
            {       "decodeModuleLayoutRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeModuleLayoutRequest(p).has_value();
             }},
            {      "decodeModuleLayoutResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeModuleLayoutResponse(p).has_value();
             }},
            {             "decodeCancelRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeCancelRequest(p).has_value();
             }},
            {            "decodeCancelResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeCancelResponse(p).has_value();
             }},
            {       "decodeVirtualQueryRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeVirtualQueryRequest(p).has_value();
             }},
            {      "decodeVirtualQueryResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeVirtualQueryResponse(p).has_value();
             }},
            {           "decodePageReadRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePageReadRequest(p).has_value();
             }},
            {              "decodePageReadData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePageReadData(p).has_value();
             }},
            {               "decodePageReadEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePageReadEnd(p).has_value();
             }},
            {           "decodePrefetchRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePrefetchRequest(p).has_value();
             }},
            {          "decodePrefetchResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePrefetchResponse(p).has_value();
             }},
            {          "decodeConfigSetRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeConfigSetRequest(p).has_value();
             }},
            {         "decodeConfigSetResponse",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeConfigSetResponse(p).has_value();
             }},
            {       "decodeMemorySearchRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeMemorySearchRequest(p).has_value();
             }},
            {          "decodeMemorySearchData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeMemorySearchData(p).has_value();
             }},
            {           "decodeMemorySearchEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeMemorySearchEnd(p).has_value();
             }},
            {         "decodeHandleListRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeHandleListRequest(p).has_value();
             }},
            {            "decodeHandleListData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeHandleListData(p).has_value();
             }},
            {             "decodeHandleListEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeHandleListEnd(p).has_value();
             }},
            {         "decodeExportListRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeExportListRequest(p).has_value();
             }},
            {            "decodeExportListData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeExportListData(p).has_value();
             }},
            {             "decodeExportListEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeExportListEnd(p).has_value();
             }},
            {         "decodeImportListRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeImportListRequest(p).has_value();
             }},
            {            "decodeImportListData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeImportListData(p).has_value();
             }},
            {             "decodeImportListEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeImportListEnd(p).has_value();
             }},
            { "decodeUnloadedModuleListRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeUnloadedModuleListRequest(p).has_value();
             }},
            {    "decodeUnloadedModuleListData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeUnloadedModuleListData(p).has_value();
             }},
            {     "decodeUnloadedModuleListEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeUnloadedModuleListEnd(p).has_value();
             }},
            {        "decodeServiceListRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeServiceListRequest(p).has_value();
             }},
            {           "decodeServiceListData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeServiceListData(p).has_value();
             }},
            {            "decodeServiceListEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeServiceListEnd(p).has_value();
             }},
            {           "decodeUserListRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeUserListRequest(p).has_value();
             }},
            {              "decodeUserListData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeUserListData(p).has_value();
             }},
            {               "decodeUserListEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeUserListEnd(p).has_value();
             }},
            {        "decodeNetworkListRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeNetworkListRequest(p).has_value();
             }},
            {           "decodeNetworkListData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeNetworkListData(p).has_value();
             }},
            {            "decodeNetworkListEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeNetworkListEnd(p).has_value();
             }},
            {           "decodeHeapListRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeHeapListRequest(p).has_value();
             }},
            {              "decodeHeapListData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeHeapListData(p).has_value();
             }},
            {               "decodeHeapListEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeHeapListEnd(p).has_value();
             }},
            {      "decodeHeapAllocListRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeHeapAllocListRequest(p).has_value();
             }},
            {         "decodeHeapAllocListData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeHeapAllocListData(p).has_value();
             }},
            {          "decodeHeapAllocListEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeHeapAllocListEnd(p).has_value();
             }},
            {        "decodeKDriverListRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeKDriverListRequest(p).has_value();
             }},
            {           "decodeKDriverListData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeKDriverListData(p).has_value();
             }},
            {            "decodeKDriverListEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeKDriverListEnd(p).has_value();
             }},
            {        "decodeKDeviceListRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeKDeviceListRequest(p).has_value();
             }},
            {           "decodeKDeviceListData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeKDeviceListData(p).has_value();
             }},
            {            "decodeKDeviceListEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeKDeviceListEnd(p).has_value();
             }},
            {        "decodeKObjectListRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeKObjectListRequest(p).has_value();
             }},
            {           "decodeKObjectListData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeKObjectListData(p).has_value();
             }},
            {            "decodeKObjectListEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodeKObjectListEnd(p).has_value();
             }},
            {            "decodePteListRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePteListRequest(p).has_value();
             }},
            {               "decodePteListData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePteListData(p).has_value();
             }},
            {                "decodePteListEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePteListEnd(p).has_value();
             }},
            {           "decodePoolListRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePoolListRequest(p).has_value();
             }},
            {              "decodePoolListData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePoolListData(p).has_value();
             }},
            {               "decodePoolListEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePoolListEnd(p).has_value();
             }},
            {            "decodePfnListRequest",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePfnListRequest(p).has_value();
             }},
            {               "decodePfnListData",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePfnListData(p).has_value();
             }},
            {                "decodePfnListEnd",
             [](std::span<const std::byte> p)
             {
             return mpfshost::decodePfnListEnd(p).has_value();
             }},
        };
    }
} // namespace

TEST_CASE("readFrame rejects truncated headers", "[fuzz][frame]")
{
    const auto encoded = mpfshost::encodeFrame(mpfshost::MessageType::BindRequest, 1, {});

    for (std::size_t length = 0; length < mpfshost::kFrameHeaderSize; ++length)
    {
        requireRejectedOrClosed(readStatus(std::span<const std::byte> {encoded.data(), length}));
    }
}

TEST_CASE("readFrame rejects a bad magic", "[fuzz][frame]")
{
    std::mt19937                            rng {0xBAD5EEDu};
    std::uniform_int_distribution<uint32_t> distribution {0, 0xFFFFFFFFu};

    for (int i = 0; i < 64; ++i)
    {
        auto magic = distribution(rng);
        if (magic == mpfshost::kProtocolMagic)
        {
            magic ^= 1;
        }
        const auto header = rawHeader(
            magic, mpfshost::kProtocolVersion, static_cast<std::uint16_t>(mpfshost::MessageType::BindRequest), 1, 0);
        requireRejectedOrClosed(readStatus(header));
    }
}

TEST_CASE("readFrame rejects an unsupported version", "[fuzz][frame]")
{
    for (const std::uint16_t version :
         {std::uint16_t {0}, static_cast<std::uint16_t>(mpfshost::kProtocolVersion + 1), std::uint16_t {0xFFFF}})
    {
        const auto header = rawHeader(
            mpfshost::kProtocolMagic, version, static_cast<std::uint16_t>(mpfshost::MessageType::BindRequest), 1, 0);
        requireRejectedOrClosed(readStatus(header));
    }
}

TEST_CASE("readFrame rejects a payload length above the cap", "[fuzz][frame]")
{
    for (const std::uint32_t length : {mpfshost::kMaxPayloadLength + 1, 0xFFFFFFFFu})
    {
        const auto header = rawHeader(mpfshost::kProtocolMagic,
                                      mpfshost::kProtocolVersion,
                                      static_cast<std::uint16_t>(mpfshost::MessageType::BindRequest),
                                      1,
                                      length);
        requireRejectedOrClosed(readStatus(header));
    }
}

TEST_CASE("readFrame survives random byte strings", "[fuzz][frame]")
{
    std::mt19937                       rng {0x1234F00Du};
    std::uniform_int_distribution<int> lengthDistribution {0, 128};

    for (int i = 0; i < 512; ++i)
    {
        const auto length = static_cast<std::size_t>(lengthDistribution(rng));
        const auto bytes  = randomBytes(rng, length);
        requireRejectedOrClosed(readStatus(bytes));
    }
}

TEST_CASE("every decoder survives random byte strings", "[fuzz][message]")
{
    std::mt19937                       rng {0xDEC0DEu};
    std::uniform_int_distribution<int> lengthDistribution {0, 96};

    const auto probes = decodeProbes();
    REQUIRE_FALSE(probes.empty());

    for (const auto& probe : probes)
    {
        for (int i = 0; i < 128; ++i)
        {
            const auto length = static_cast<std::size_t>(lengthDistribution(rng));
            const auto bytes  = randomBytes(rng, length);
            CHECK_NOTHROW(probe.second(bytes));
        }
    }
}
