#include "core/ipc/message.hpp"
#include "core/ipc/message_codec.hpp"

namespace mpfshost
{
    using namespace detail;

    std::vector<std::byte> encodeProcessListRequest(const ProcessFilter& filter)
    {
        ByteWriter writer;
        writer.writeU32(filter.pid);
        writer.writeString(filter.nameFilter);
        return writer.take();
    }

    std::optional<ProcessFilter> decodeProcessListRequest(std::span<const std::byte> payload)
    {
        ByteReader    reader {payload};
        ProcessFilter filter;
        filter.pid        = reader.readU32();
        filter.nameFilter = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return filter;
    }

    std::vector<std::byte> encodeProcessListResponse(const ProcessListResponse& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.processes.size()));
        for (const auto& process : message.processes)
        {
            writeProcess(writer, process);
        }
        return writer.take();
    }

    std::optional<ProcessListResponse> decodeProcessListResponse(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        ProcessListResponse message;
        message.processes.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            ProcessDescriptor process;
            if (!readProcess(reader, process))
            {
                return std::nullopt;
            }
            message.processes.push_back(std::move(process));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeProcessListAllResponse(const ProcessListAllResponse& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.processes.size()));
        for (const auto& summary : message.processes)
        {
            writeProcess(writer, summary.process);
            writer.writeU32(summary.state);
        }
        return writer.take();
    }

    std::optional<ProcessListAllResponse> decodeProcessListAllResponse(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        ProcessListAllResponse message;
        message.processes.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            ProcessSummary summary;
            if (!readProcess(reader, summary.process))
            {
                return std::nullopt;
            }
            summary.state = reader.readU32();
            message.processes.push_back(std::move(summary));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeProcessInfoRequest(const ProcessInfoRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeString(message.name);
        return writer.take();
    }

    std::optional<ProcessInfoRequest> decodeProcessInfoRequest(std::span<const std::byte> payload)
    {
        ByteReader         reader {payload};
        ProcessInfoRequest message;
        message.pid = reader.readU32();
        if (!readCappedString(reader, message.name) || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeProcessInfoResponse(const ProcessInfoResponse& message)
    {
        ByteWriter writer;
        writeProcess(writer, message.process);
        writer.writeU32(message.state);
        writer.writeU16(message.memoryModel);
        writer.writeU16(message.systemType);
        writer.writeU8(message.userOnly ? 1 : 0);
        writer.writeU64(message.dtb);
        writer.writeU64(message.dtbUser);
        writer.writeU64(message.eprocessVa);
        writer.writeU64(message.pebVa);
        writer.writeU32(message.peb32Va);
        writer.writeU32(message.sessionId);
        writer.writeU64(message.luid);
        writer.writeU32(message.integrityLevel);
        writer.writeString(message.sid);
        writer.writeString(message.imagePath);
        writer.writeString(message.kernelPath);
        writer.writeString(message.cmdline);
        return writer.take();
    }

    std::optional<ProcessInfoResponse> decodeProcessInfoResponse(std::span<const std::byte> payload)
    {
        ByteReader          reader {payload};
        ProcessInfoResponse message;
        if (!readProcess(reader, message.process))
        {
            return std::nullopt;
        }
        message.state          = reader.readU32();
        message.memoryModel    = reader.readU16();
        message.systemType     = reader.readU16();
        message.userOnly       = reader.readU8() != 0;
        message.dtb            = reader.readU64();
        message.dtbUser        = reader.readU64();
        message.eprocessVa     = reader.readU64();
        message.pebVa          = reader.readU64();
        message.peb32Va        = reader.readU32();
        message.sessionId      = reader.readU32();
        message.luid           = reader.readU64();
        message.integrityLevel = reader.readU32();
        if (!readCappedString(reader, message.sid) || !readCappedString(reader, message.imagePath)
            || !readCappedString(reader, message.kernelPath) || !readCappedString(reader, message.cmdline)
            || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeProcessByDtbRequest(const ProcessByDtbRequest& message)
    {
        ByteWriter writer;
        writer.writeU64(message.dtb);
        return writer.take();
    }

    std::optional<ProcessByDtbRequest> decodeProcessByDtbRequest(std::span<const std::byte> payload)
    {
        ByteReader          reader {payload};
        ProcessByDtbRequest message;
        message.dtb = reader.readU64();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeProcessByDtbResponse(const ProcessByDtbResponse& message)
    {
        ByteWriter writer;
        writeProcess(writer, message.process);
        writer.writeU64(message.dtb);
        writer.writeU64(message.dtbUser);
        writer.writeU8(message.matchedUser ? 1 : 0);
        return writer.take();
    }

    std::optional<ProcessByDtbResponse> decodeProcessByDtbResponse(std::span<const std::byte> payload)
    {
        ByteReader           reader {payload};
        ProcessByDtbResponse message;
        if (!readProcess(reader, message.process))
        {
            return std::nullopt;
        }
        message.dtb         = reader.readU64();
        message.dtbUser     = reader.readU64();
        message.matchedUser = reader.readU8() != 0;
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeSymbolLookupRequest(const SymbolLookupRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeString(message.module);
        writer.writeString(message.symbol);
        return writer.take();
    }

    std::optional<SymbolLookupRequest> decodeSymbolLookupRequest(std::span<const std::byte> payload)
    {
        ByteReader          reader {payload};
        SymbolLookupRequest message;
        message.pid = reader.readU32();
        if (!readCappedString(reader, message.module) || !readCappedString(reader, message.symbol) || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeSymbolLookupResponse(const SymbolLookupResponse& message)
    {
        ByteWriter writer;
        writer.writeU64(message.moduleBase);
        writer.writeU64(message.address);
        writer.writeU64(message.offset);
        writer.writeU8(message.found ? 1 : 0);
        return writer.take();
    }

    std::optional<SymbolLookupResponse> decodeSymbolLookupResponse(std::span<const std::byte> payload)
    {
        ByteReader           reader {payload};
        SymbolLookupResponse message;
        message.moduleBase = reader.readU64();
        message.address    = reader.readU64();
        message.offset     = reader.readU64();
        message.found      = reader.readU8() != 0;
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePdbLoadRequest(const PdbLoadRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeU64(message.moduleBase);
        return writer.take();
    }

    std::optional<PdbLoadRequest> decodePdbLoadRequest(std::span<const std::byte> payload)
    {
        ByteReader     reader {payload};
        PdbLoadRequest message;
        message.pid        = reader.readU32();
        message.moduleBase = reader.readU64();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePdbLoadResponse(const PdbLoadResponse& message)
    {
        ByteWriter writer;
        writer.writeString(message.module);
        return writer.take();
    }

    std::optional<PdbLoadResponse> decodePdbLoadResponse(std::span<const std::byte> payload)
    {
        ByteReader      reader {payload};
        PdbLoadResponse message;
        if (!readCappedString(reader, message.module) || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePdbSymbolNameRequest(const PdbSymbolNameRequest& message)
    {
        ByteWriter writer;
        writer.writeString(message.module);
        writer.writeU64(message.address);
        return writer.take();
    }

    std::optional<PdbSymbolNameRequest> decodePdbSymbolNameRequest(std::span<const std::byte> payload)
    {
        ByteReader           reader {payload};
        PdbSymbolNameRequest message;
        if (!readCappedString(reader, message.module))
        {
            return std::nullopt;
        }
        message.address = reader.readU64();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePdbSymbolNameResponse(const PdbSymbolNameResponse& message)
    {
        ByteWriter writer;
        writer.writeString(message.symbol);
        writer.writeU32(message.displacement);
        return writer.take();
    }

    std::optional<PdbSymbolNameResponse> decodePdbSymbolNameResponse(std::span<const std::byte> payload)
    {
        ByteReader            reader {payload};
        PdbSymbolNameResponse message;
        if (!readCappedString(reader, message.symbol))
        {
            return std::nullopt;
        }
        message.displacement = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePdbSymbolAddressRequest(const PdbSymbolAddressRequest& message)
    {
        ByteWriter writer;
        writer.writeString(message.module);
        writer.writeString(message.symbol);
        return writer.take();
    }

    std::optional<PdbSymbolAddressRequest> decodePdbSymbolAddressRequest(std::span<const std::byte> payload)
    {
        ByteReader              reader {payload};
        PdbSymbolAddressRequest message;
        if (!readCappedString(reader, message.module) || !readCappedString(reader, message.symbol) || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePdbSymbolAddressResponse(const PdbSymbolAddressResponse& message)
    {
        ByteWriter writer;
        writer.writeU64(message.address);
        return writer.take();
    }

    std::optional<PdbSymbolAddressResponse> decodePdbSymbolAddressResponse(std::span<const std::byte> payload)
    {
        ByteReader               reader {payload};
        PdbSymbolAddressResponse message;
        message.address = reader.readU64();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePdbTypeSizeRequest(const PdbTypeSizeRequest& message)
    {
        ByteWriter writer;
        writer.writeString(message.module);
        writer.writeString(message.type);
        return writer.take();
    }

    std::optional<PdbTypeSizeRequest> decodePdbTypeSizeRequest(std::span<const std::byte> payload)
    {
        ByteReader         reader {payload};
        PdbTypeSizeRequest message;
        if (!readCappedString(reader, message.module) || !readCappedString(reader, message.type) || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePdbTypeSizeResponse(const PdbTypeSizeResponse& message)
    {
        ByteWriter writer;
        writer.writeU32(message.size);
        return writer.take();
    }

    std::optional<PdbTypeSizeResponse> decodePdbTypeSizeResponse(std::span<const std::byte> payload)
    {
        ByteReader          reader {payload};
        PdbTypeSizeResponse message;
        message.size = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePdbTypeChildOffsetRequest(const PdbTypeChildOffsetRequest& message)
    {
        ByteWriter writer;
        writer.writeString(message.module);
        writer.writeString(message.type);
        writer.writeString(message.child);
        return writer.take();
    }

    std::optional<PdbTypeChildOffsetRequest> decodePdbTypeChildOffsetRequest(std::span<const std::byte> payload)
    {
        ByteReader                reader {payload};
        PdbTypeChildOffsetRequest message;
        if (!readCappedString(reader, message.module) || !readCappedString(reader, message.type)
            || !readCappedString(reader, message.child) || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePdbTypeChildOffsetResponse(const PdbTypeChildOffsetResponse& message)
    {
        ByteWriter writer;
        writer.writeU32(message.offset);
        return writer.take();
    }

    std::optional<PdbTypeChildOffsetResponse> decodePdbTypeChildOffsetResponse(std::span<const std::byte> payload)
    {
        ByteReader                 reader {payload};
        PdbTypeChildOffsetResponse message;
        message.offset = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeHandleListRequest(const HandleListRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeString(message.filter);
        return writer.take();
    }

    std::optional<HandleListRequest> decodeHandleListRequest(std::span<const std::byte> payload)
    {
        ByteReader        reader {payload};
        HandleListRequest message;
        message.pid    = reader.readU32();
        message.filter = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeHandleListData(const HandleListData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.handles.size()));
        for (const auto& handle : message.handles)
        {
            writeHandle(writer, handle);
        }
        return writer.take();
    }

    std::optional<HandleListData> decodeHandleListData(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        HandleListData message;
        message.handles.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            HandleDescriptor handle;
            if (!readHandle(reader, handle))
            {
                return std::nullopt;
            }
            message.handles.push_back(std::move(handle));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeHandleListEnd(const HandleListEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.handleCount);
        writer.writeU8(message.truncated);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<HandleListEnd> decodeHandleListEnd(std::span<const std::byte> payload)
    {
        ByteReader    reader {payload};
        HandleListEnd message;
        message.status      = reader.readU8();
        message.handleCount = reader.readU64();
        message.truncated   = reader.readU8();
        message.error       = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

} // namespace mpfshost
