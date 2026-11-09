#include "core/ipc/frame.hpp"

#include <cerrno>
#include <cstring>
#include <utility>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mpfshost
{
    namespace
    {
        enum class IoStatus
        {
            Ok,
            Eof,
            TimedOut,
            Error,
        };

        IoStatus readAll(int fd, void* buffer, std::size_t count, std::size_t& received, std::string& error)
        {
            auto* bytes = static_cast<std::byte*>(buffer);
            received    = 0;
            while (received < count)
            {
                const auto result = ::read(fd, bytes + received, count - received);
                if (result < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        return IoStatus::TimedOut;
                    }
                    error = std::string("read failed: ") + std::strerror(errno);
                    return IoStatus::Error;
                }
                if (result == 0)
                {
                    return IoStatus::Eof;
                }
                received += static_cast<std::size_t>(result);
            }
            return IoStatus::Ok;
        }

        ssize_t writeSome(int fd, const std::byte* data, std::size_t count)
        {
            // MSG_NOSIGNAL keeps a vanished peer from terminating the process with SIGPIPE.
            const auto result = ::send(fd, data, count, MSG_NOSIGNAL);
            if (result < 0 && errno == ENOTSOCK)
            {
                return ::write(fd, data, count);
            }
            return result;
        }
    } // namespace

    void ByteWriter::writeU8(std::uint8_t value)
    {
        _buffer.push_back(static_cast<std::byte>(value));
    }

    void ByteWriter::writeU16(std::uint16_t value)
    {
        for (unsigned shift = 0; shift < 16; shift += 8)
        {
            _buffer.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
        }
    }

    void ByteWriter::writeU32(std::uint32_t value)
    {
        for (unsigned shift = 0; shift < 32; shift += 8)
        {
            _buffer.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
        }
    }

    void ByteWriter::writeU64(std::uint64_t value)
    {
        for (unsigned shift = 0; shift < 64; shift += 8)
        {
            _buffer.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
        }
    }

    void ByteWriter::writeString(std::string_view value)
    {
        writeU32(static_cast<std::uint32_t>(value.size()));
        _buffer.insert(_buffer.end(),
                       reinterpret_cast<const std::byte*>(value.data()),
                       reinterpret_cast<const std::byte*>(value.data()) + value.size());
    }

    void ByteWriter::writeBytes(std::span<const std::byte> value)
    {
        writeU32(static_cast<std::uint32_t>(value.size()));
        _buffer.insert(_buffer.end(), value.begin(), value.end());
    }

    const std::vector<std::byte>& ByteWriter::bytes() const noexcept
    {
        return _buffer;
    }

    std::vector<std::byte> ByteWriter::take()
    {
        return std::move(_buffer);
    }

    ByteReader::ByteReader(std::span<const std::byte> data) noexcept : _data {data} {}

    bool ByteReader::readBytes(std::byte* destination, std::size_t count)
    {
        if (_failed || count > _data.size() - _offset)
        {
            _failed = true;
            return false;
        }
        std::memcpy(destination, _data.data() + _offset, count);
        _offset += count;
        return true;
    }

    std::uint8_t ByteReader::readU8()
    {
        std::byte raw {};
        if (!readBytes(&raw, sizeof(raw)))
        {
            return 0;
        }
        return std::to_integer<std::uint8_t>(raw);
    }

    std::uint16_t ByteReader::readU16()
    {
        std::byte raw[2] {};
        if (!readBytes(raw, sizeof(raw)))
        {
            return 0;
        }
        std::uint16_t value = 0;
        for (std::size_t i = 0; i < sizeof(raw); ++i)
        {
            value |= static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(raw[i]) << (8 * i));
        }
        return value;
    }

    std::uint32_t ByteReader::readU32()
    {
        std::byte raw[4] {};
        if (!readBytes(raw, sizeof(raw)))
        {
            return 0;
        }
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < sizeof(raw); ++i)
        {
            value |= static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(raw[i])) << (8 * i);
        }
        return value;
    }

    std::uint64_t ByteReader::readU64()
    {
        std::byte raw[8] {};
        if (!readBytes(raw, sizeof(raw)))
        {
            return 0;
        }
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < sizeof(raw); ++i)
        {
            value |= static_cast<std::uint64_t>(std::to_integer<std::uint64_t>(raw[i])) << (8 * i);
        }
        return value;
    }

    std::string ByteReader::readString()
    {
        const auto length = readU32();
        if (_failed || length > _data.size() - _offset)
        {
            _failed = true;
            return {};
        }
        std::string result(reinterpret_cast<const char*>(_data.data() + _offset), length);
        _offset += length;
        return result;
    }

    std::vector<std::byte> ByteReader::readBytes()
    {
        const auto length = readU32();
        if (_failed || length > _data.size() - _offset)
        {
            _failed = true;
            return {};
        }
        std::vector<std::byte> result(_data.begin() + static_cast<std::ptrdiff_t>(_offset),
                                      _data.begin() + static_cast<std::ptrdiff_t>(_offset + length));
        _offset += length;
        return result;
    }

    bool ByteReader::ok() const noexcept
    {
        return !_failed;
    }

    bool ByteReader::atEnd() const noexcept
    {
        return _offset == _data.size();
    }

    std::vector<std::byte> encodeFrame(MessageType type, std::uint64_t requestId, std::span<const std::byte> payload)
    {
        ByteWriter writer;
        writer.writeU32(kProtocolMagic);
        writer.writeU16(kProtocolVersion);
        writer.writeU16(static_cast<std::uint16_t>(type));
        writer.writeU64(requestId);
        writer.writeU32(static_cast<std::uint32_t>(payload.size()));

        auto frame = writer.take();
        frame.insert(frame.end(), payload.begin(), payload.end());
        return frame;
    }

    FrameReadStatus readFrame(int fd, FrameHeader& header, std::vector<std::byte>& payload, std::string& error)
    {
        payload.clear();
        error.clear();

        std::byte   rawHeader[kFrameHeaderSize] {};
        std::size_t headerReceived = 0;
        const auto  headerStatus   = readAll(fd, rawHeader, kFrameHeaderSize, headerReceived, error);
        if (headerStatus == IoStatus::TimedOut)
        {
            error = "timed out while reading the frame header";
            return FrameReadStatus::Timeout;
        }
        if (headerStatus == IoStatus::Error)
        {
            return FrameReadStatus::Closed;
        }
        if (headerStatus == IoStatus::Eof)
        {
            if (headerReceived > 0)
            {
                error = "connection closed while reading the frame header";
            }
            return FrameReadStatus::Closed;
        }

        ByteReader reader {
            std::span<const std::byte> {rawHeader, kFrameHeaderSize}
        };
        const auto magic         = reader.readU32();
        const auto version       = reader.readU16();
        const auto type          = static_cast<MessageType>(reader.readU16());
        const auto requestId     = reader.readU64();
        const auto payloadLength = reader.readU32();
        if (!reader.ok())
        {
            error = "malformed frame header";
            return FrameReadStatus::Closed;
        }
        if (magic != kProtocolMagic)
        {
            error = "invalid frame magic";
            return FrameReadStatus::Closed;
        }

        header.version       = version;
        header.type          = type;
        header.requestId     = requestId;
        header.payloadLength = payloadLength;

        if (version != kProtocolVersion)
        {
            error = "unsupported protocol version " + std::to_string(version);
            return FrameReadStatus::Rejected;
        }
        if (payloadLength > kMaxPayloadLength)
        {
            error = "payload length " + std::to_string(payloadLength) + " exceeds the limit";
            return FrameReadStatus::Rejected;
        }

        payload.resize(payloadLength);
        if (payloadLength > 0)
        {
            std::size_t payloadReceived = 0;
            const auto  payloadStatus   = readAll(fd, payload.data(), payloadLength, payloadReceived, error);
            if (payloadStatus != IoStatus::Ok)
            {
                payload.clear();
                if (payloadStatus == IoStatus::TimedOut)
                {
                    error = "timed out while reading the frame payload";
                    return FrameReadStatus::Timeout;
                }
                if (payloadStatus == IoStatus::Eof && error.empty())
                {
                    error = "connection closed while reading the frame payload";
                }
                return FrameReadStatus::Closed;
            }
        }
        return FrameReadStatus::Complete;
    }

    namespace
    {
        std::optional<FrameHeader> peekBufferedHeader(int fd)
        {
            std::byte  rawHeader[kFrameHeaderSize] {};
            const auto headerResult = ::recv(fd, rawHeader, kFrameHeaderSize, MSG_PEEK | MSG_DONTWAIT);
            if (headerResult < static_cast<ssize_t>(kFrameHeaderSize))
            {
                return std::nullopt;
            }

            ByteReader reader {
                std::span<const std::byte> {rawHeader, kFrameHeaderSize}
            };
            const auto magic         = reader.readU32();
            const auto version       = reader.readU16();
            const auto type          = reader.readU16();
            const auto requestId     = reader.readU64();
            const auto payloadLength = reader.readU32();
            if (!reader.ok() || magic != kProtocolMagic || payloadLength > kMaxPayloadLength)
            {
                return std::nullopt;
            }

            // FIONREAD keeps the check allocation-free for a large announced payload; MSG_PEEK would need a
            // buffer as long as the whole frame just to learn whether it is complete.
            int available = 0;
            if (::ioctl(fd, FIONREAD, &available) != 0)
            {
                return std::nullopt;
            }
            const auto needed = static_cast<std::uint64_t>(kFrameHeaderSize) + payloadLength;
            if (static_cast<std::uint64_t>(available) < needed)
            {
                return std::nullopt;
            }
            return FrameHeader {version, static_cast<MessageType>(type), requestId, payloadLength};
        }
    } // namespace

    std::optional<FrameHeader> bufferedFrameHeader(int fd)
    {
        return peekBufferedHeader(fd);
    }

    std::optional<std::uint32_t> bufferedFrameLength(int fd)
    {
        const auto header = peekBufferedHeader(fd);
        if (!header)
        {
            return std::nullopt;
        }
        return header->payloadLength;
    }

    bool writeFrame(
        int fd, MessageType type, std::uint64_t requestId, std::span<const std::byte> payload, std::string& error)
    {
        const auto  frame = encodeFrame(type, requestId, payload);
        std::size_t written {};
        while (written < frame.size())
        {
            const auto result = writeSome(fd, frame.data() + written, frame.size() - written);
            if (result < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                error = std::string("write failed: ") + std::strerror(errno);
                return false;
            }
            written += static_cast<std::size_t>(result);
        }
        error.clear();
        return true;
    }

    std::string_view messageTypeName(MessageType type) noexcept
    {
        switch (type)
        {
        case MessageType::ListTargetsRequest:
            return "ListTargetsRequest";
        case MessageType::BindRequest:
            return "BindRequest";
        case MessageType::ProcessListRequest:
            return "ProcessListRequest";
        case MessageType::MemoryReadRequest:
            return "MemoryReadRequest";
        case MessageType::MemoryWriteRequest:
            return "MemoryWriteRequest";
        case MessageType::ScatterCreateRequest:
            return "ScatterCreateRequest";
        case MessageType::ScatterPrepareRequest:
            return "ScatterPrepareRequest";
        case MessageType::ScatterExecuteRequest:
            return "ScatterExecuteRequest";
        case MessageType::ScatterCloseRequest:
            return "ScatterCloseRequest";
        case MessageType::ScatterExecuteReadRequest:
            return "ScatterExecuteReadRequest";
        case MessageType::ScatterClearRequest:
            return "ScatterClearRequest";
        case MessageType::ModuleListRequest:
            return "ModuleListRequest";
        case MessageType::MemoryMapRequest:
            return "MemoryMapRequest";
        case MessageType::ThreadListRequest:
            return "ThreadListRequest";
        case MessageType::VmListRequest:
            return "VmListRequest";
        case MessageType::PhysicalReadRequest:
            return "PhysicalReadRequest";
        case MessageType::PhysicalWriteRequest:
            return "PhysicalWriteRequest";
        case MessageType::GpaTranslateRequest:
            return "GpaTranslateRequest";
        case MessageType::Virt2PhysRequest:
            return "Virt2PhysRequest";
        case MessageType::PhysicalScatterRequest:
            return "PhysicalScatterRequest";
        case MessageType::VfsListRequest:
            return "VfsListRequest";
        case MessageType::VfsReadRequest:
            return "VfsReadRequest";
        case MessageType::VfsWriteRequest:
            return "VfsWriteRequest";
        case MessageType::ModuleByAddressRequest:
            return "ModuleByAddressRequest";
        case MessageType::ProcessByDtbRequest:
            return "ProcessByDtbRequest";
        case MessageType::DebugStateRequest:
            return "DebugStateRequest";
        case MessageType::DebugControlRequest:
            return "DebugControlRequest";
        case MessageType::DebugRegistersRequest:
            return "DebugRegistersRequest";
        case MessageType::DebugWatchRequest:
            return "DebugWatchRequest";
        case MessageType::DebugBreakpointRequest:
            return "DebugBreakpointRequest";
        case MessageType::DebugStepRequest:
            return "DebugStepRequest";
        case MessageType::DebugDetachRequest:
            return "DebugDetachRequest";
        case MessageType::DebugAttachRequest:
            return "DebugAttachRequest";
        case MessageType::DebugBreakpointArmRequest:
            return "DebugBreakpointArmRequest";
        case MessageType::DebugBreakpointRemoveRequest:
            return "DebugBreakpointRemoveRequest";
        case MessageType::DebugBreakpointListRequest:
            return "DebugBreakpointListRequest";
        case MessageType::DebugWatchArmRequest:
            return "DebugWatchArmRequest";
        case MessageType::DebugWatchRemoveRequest:
            return "DebugWatchRemoveRequest";
        case MessageType::DebugWatchListRequest:
            return "DebugWatchListRequest";
        case MessageType::DebugContinueRequest:
            return "DebugContinueRequest";
        case MessageType::DebugWaitRequest:
            return "DebugWaitRequest";
        case MessageType::DebugThreadsRequest:
            return "DebugThreadsRequest";
        case MessageType::DisassembleRequest:
            return "DisassembleRequest";
        case MessageType::DebugBacktraceRequest:
            return "DebugBacktraceRequest";
        case MessageType::DebugWatchData:
            return "DebugWatchData";
        case MessageType::DebugWatchEnd:
            return "DebugWatchEnd";
        case MessageType::DebugBreakpointResponse:
            return "DebugBreakpointResponse";
        case MessageType::DebugStepResponse:
            return "DebugStepResponse";
        case MessageType::DebugDetachResponse:
            return "DebugDetachResponse";
        case MessageType::DebugAttachResponse:
            return "DebugAttachResponse";
        case MessageType::ProcessInfoRequest:
            return "ProcessInfoRequest";
        case MessageType::SymbolLookupRequest:
            return "SymbolLookupRequest";
        case MessageType::ModuleLayoutRequest:
            return "ModuleLayoutRequest";
        case MessageType::CancelRequest:
            return "CancelRequest";
        case MessageType::VirtualQueryRequest:
            return "VirtualQueryRequest";
        case MessageType::MemorySearchRequest:
            return "MemorySearchRequest";
        case MessageType::PageReadRequest:
            return "PageReadRequest";
        case MessageType::PrefetchRequest:
            return "PrefetchRequest";
        case MessageType::HandleListRequest:
            return "HandleListRequest";
        case MessageType::ExportListRequest:
            return "ExportListRequest";
        case MessageType::ImportListRequest:
            return "ImportListRequest";
        case MessageType::UnloadedModuleListRequest:
            return "UnloadedModuleListRequest";
        case MessageType::ServiceListRequest:
            return "ServiceListRequest";
        case MessageType::UserListRequest:
            return "UserListRequest";
        case MessageType::NetworkListRequest:
            return "NetworkListRequest";
        case MessageType::HeapListRequest:
            return "HeapListRequest";
        case MessageType::HeapAllocListRequest:
            return "HeapAllocListRequest";
        case MessageType::KDriverListRequest:
            return "KDriverListRequest";
        case MessageType::KDeviceListRequest:
            return "KDeviceListRequest";
        case MessageType::KObjectListRequest:
            return "KObjectListRequest";
        case MessageType::PteListRequest:
            return "PteListRequest";
        case MessageType::PoolListRequest:
            return "PoolListRequest";
        case MessageType::PoolTagListRequest:
            return "PoolTagListRequest";
        case MessageType::ConfigSetRequest:
            return "ConfigSetRequest";
        case MessageType::PfnListRequest:
            return "PfnListRequest";
        case MessageType::CapabilitiesRequest:
            return "CapabilitiesRequest";
        case MessageType::PdbLoadRequest:
            return "PdbLoadRequest";
        case MessageType::PdbSymbolNameRequest:
            return "PdbSymbolNameRequest";
        case MessageType::PdbSymbolAddressRequest:
            return "PdbSymbolAddressRequest";
        case MessageType::PdbTypeSizeRequest:
            return "PdbTypeSizeRequest";
        case MessageType::PdbTypeChildOffsetRequest:
            return "PdbTypeChildOffsetRequest";
        case MessageType::ProcessListAllRequest:
            return "ProcessListAllRequest";
        case MessageType::VadExListRequest:
            return "VadExListRequest";
        case MessageType::ListTargetsResponse:
            return "ListTargetsResponse";
        case MessageType::BindResponse:
            return "BindResponse";
        case MessageType::ProcessListResponse:
            return "ProcessListResponse";
        case MessageType::MemoryReadData:
            return "MemoryReadData";
        case MessageType::MemoryReadEnd:
            return "MemoryReadEnd";
        case MessageType::MemoryWriteResponse:
            return "MemoryWriteResponse";
        case MessageType::ScatterCreateResponse:
            return "ScatterCreateResponse";
        case MessageType::ScatterPrepareResponse:
            return "ScatterPrepareResponse";
        case MessageType::ScatterExecuteData:
            return "ScatterExecuteData";
        case MessageType::ScatterExecuteEnd:
            return "ScatterExecuteEnd";
        case MessageType::ScatterCloseResponse:
            return "ScatterCloseResponse";
        case MessageType::ScatterExecuteReadData:
            return "ScatterExecuteReadData";
        case MessageType::ScatterExecuteReadEnd:
            return "ScatterExecuteReadEnd";
        case MessageType::ScatterClearResponse:
            return "ScatterClearResponse";
        case MessageType::ModuleListData:
            return "ModuleListData";
        case MessageType::ModuleListEnd:
            return "ModuleListEnd";
        case MessageType::MemoryMapData:
            return "MemoryMapData";
        case MessageType::MemoryMapEnd:
            return "MemoryMapEnd";
        case MessageType::ThreadListData:
            return "ThreadListData";
        case MessageType::ThreadListEnd:
            return "ThreadListEnd";
        case MessageType::VmListResponse:
            return "VmListResponse";
        case MessageType::PhysicalReadData:
            return "PhysicalReadData";
        case MessageType::PhysicalReadEnd:
            return "PhysicalReadEnd";
        case MessageType::PhysicalWriteResponse:
            return "PhysicalWriteResponse";
        case MessageType::GpaTranslateResponse:
            return "GpaTranslateResponse";
        case MessageType::Virt2PhysResponse:
            return "Virt2PhysResponse";
        case MessageType::PhysicalScatterData:
            return "PhysicalScatterData";
        case MessageType::PhysicalScatterEnd:
            return "PhysicalScatterEnd";
        case MessageType::VfsListData:
            return "VfsListData";
        case MessageType::VfsListEnd:
            return "VfsListEnd";
        case MessageType::VfsReadData:
            return "VfsReadData";
        case MessageType::VfsReadEnd:
            return "VfsReadEnd";
        case MessageType::VfsWriteResponse:
            return "VfsWriteResponse";
        case MessageType::ModuleByAddressResponse:
            return "ModuleByAddressResponse";
        case MessageType::ProcessByDtbResponse:
            return "ProcessByDtbResponse";
        case MessageType::DebugStateResponse:
            return "DebugStateResponse";
        case MessageType::DebugControlResponse:
            return "DebugControlResponse";
        case MessageType::DebugRegistersResponse:
            return "DebugRegistersResponse";
        case MessageType::DebugBreakpointArmResponse:
            return "DebugBreakpointArmResponse";
        case MessageType::DebugResourceRemoveResponse:
            return "DebugResourceRemoveResponse";
        case MessageType::DebugBreakpointListResponse:
            return "DebugBreakpointListResponse";
        case MessageType::DebugWatchArmResponse:
            return "DebugWatchArmResponse";
        case MessageType::DebugWatchListResponse:
            return "DebugWatchListResponse";
        case MessageType::DebugContinueResponse:
            return "DebugContinueResponse";
        case MessageType::DebugWaitHit:
            return "DebugWaitHit";
        case MessageType::DebugWaitEnd:
            return "DebugWaitEnd";
        case MessageType::DebugThreadsResponse:
            return "DebugThreadsResponse";
        case MessageType::DisassembleResponse:
            return "DisassembleResponse";
        case MessageType::DebugBacktraceResponse:
            return "DebugBacktraceResponse";
        case MessageType::DebugWatchHeartbeat:
            return "DebugWatchHeartbeat";
        case MessageType::ProcessInfoResponse:
            return "ProcessInfoResponse";
        case MessageType::SymbolLookupResponse:
            return "SymbolLookupResponse";
        case MessageType::ModuleLayoutResponse:
            return "ModuleLayoutResponse";
        case MessageType::CancelResponse:
            return "CancelResponse";
        case MessageType::VirtualQueryResponse:
            return "VirtualQueryResponse";
        case MessageType::MemorySearchData:
            return "MemorySearchData";
        case MessageType::MemorySearchEnd:
            return "MemorySearchEnd";
        case MessageType::PageReadData:
            return "PageReadData";
        case MessageType::PageReadEnd:
            return "PageReadEnd";
        case MessageType::PrefetchResponse:
            return "PrefetchResponse";
        case MessageType::HandleListData:
            return "HandleListData";
        case MessageType::HandleListEnd:
            return "HandleListEnd";
        case MessageType::ExportListData:
            return "ExportListData";
        case MessageType::ExportListEnd:
            return "ExportListEnd";
        case MessageType::ImportListData:
            return "ImportListData";
        case MessageType::ImportListEnd:
            return "ImportListEnd";
        case MessageType::UnloadedModuleListData:
            return "UnloadedModuleListData";
        case MessageType::UnloadedModuleListEnd:
            return "UnloadedModuleListEnd";
        case MessageType::ServiceListData:
            return "ServiceListData";
        case MessageType::ServiceListEnd:
            return "ServiceListEnd";
        case MessageType::UserListData:
            return "UserListData";
        case MessageType::UserListEnd:
            return "UserListEnd";
        case MessageType::NetworkListData:
            return "NetworkListData";
        case MessageType::NetworkListEnd:
            return "NetworkListEnd";
        case MessageType::HeapListData:
            return "HeapListData";
        case MessageType::HeapListEnd:
            return "HeapListEnd";
        case MessageType::HeapAllocListData:
            return "HeapAllocListData";
        case MessageType::HeapAllocListEnd:
            return "HeapAllocListEnd";
        case MessageType::KDriverListData:
            return "KDriverListData";
        case MessageType::KDriverListEnd:
            return "KDriverListEnd";
        case MessageType::KDeviceListData:
            return "KDeviceListData";
        case MessageType::KDeviceListEnd:
            return "KDeviceListEnd";
        case MessageType::KObjectListData:
            return "KObjectListData";
        case MessageType::KObjectListEnd:
            return "KObjectListEnd";
        case MessageType::PteListData:
            return "PteListData";
        case MessageType::PteListEnd:
            return "PteListEnd";
        case MessageType::PoolListData:
            return "PoolListData";
        case MessageType::PoolListEnd:
            return "PoolListEnd";
        case MessageType::PoolTagListData:
            return "PoolTagListData";
        case MessageType::PoolTagListEnd:
            return "PoolTagListEnd";
        case MessageType::ConfigSetResponse:
            return "ConfigSetResponse";
        case MessageType::PfnListData:
            return "PfnListData";
        case MessageType::PfnListEnd:
            return "PfnListEnd";
        case MessageType::CapabilitiesResponse:
            return "CapabilitiesResponse";
        case MessageType::PdbLoadResponse:
            return "PdbLoadResponse";
        case MessageType::PdbSymbolNameResponse:
            return "PdbSymbolNameResponse";
        case MessageType::PdbSymbolAddressResponse:
            return "PdbSymbolAddressResponse";
        case MessageType::PdbTypeSizeResponse:
            return "PdbTypeSizeResponse";
        case MessageType::PdbTypeChildOffsetResponse:
            return "PdbTypeChildOffsetResponse";
        case MessageType::ProcessListAllResponse:
            return "ProcessListAllResponse";
        case MessageType::VadExListData:
            return "VadExListData";
        case MessageType::VadExListEnd:
            return "VadExListEnd";
        case MessageType::ErrorResponse:
            return "ErrorResponse";
        }
        return "UnknownMessageType";
    }

    std::string_view errorCodeName(ErrorCode code) noexcept
    {
        switch (code)
        {
        case ErrorCode::MalformedPayload:
            return "MalformedPayload";
        case ErrorCode::UnsupportedVersion:
            return "UnsupportedVersion";
        case ErrorCode::UnknownMessage:
            return "UnknownMessage";
        case ErrorCode::TargetNotFound:
            return "TargetNotFound";
        case ErrorCode::TargetUnavailable:
            return "TargetUnavailable";
        case ErrorCode::SessionAlreadyBound:
            return "SessionAlreadyBound";
        case ErrorCode::Internal:
            return "Internal";
        case ErrorCode::NotBound:
            return "NotBound";
        case ErrorCode::CapabilityUnavailable:
            return "CapabilityUnavailable";
        case ErrorCode::ResultTooLarge:
            return "ResultTooLarge";
        case ErrorCode::UnknownScatter:
            return "UnknownScatter";
        case ErrorCode::InvalidRange:
            return "InvalidRange";
        case ErrorCode::ScatterLimitReached:
            return "ScatterLimitReached";
        case ErrorCode::UnknownVm:
            return "UnknownVm";
        case ErrorCode::NotFound:
            return "NotFound";
        case ErrorCode::AccessDenied:
            return "AccessDenied";
        case ErrorCode::Cancelled:
            return "Cancelled";
        case ErrorCode::Timeout:
            return "Timeout";
        case ErrorCode::Busy:
            return "Busy";
        case ErrorCode::InvalidState:
            return "InvalidState";
        }
        return "UnknownErrorCode";
    }
} // namespace mpfshost
