#include "core/ipc/message.hpp"
#include "core/ipc/message_codec.hpp"

namespace mpfshost
{
    using namespace detail;

    std::vector<std::byte> encodeMemoryReadRequest(const MemoryReadRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeU64(message.address);
        writer.writeU32(message.size);
        writer.writeU8(message.flags);
        return writer.take();
    }

    std::optional<MemoryReadRequest> decodeMemoryReadRequest(std::span<const std::byte> payload)
    {
        ByteReader        reader {payload};
        MemoryReadRequest message;
        message.pid     = reader.readU32();
        message.address = reader.readU64();
        message.size    = reader.readU32();
        message.flags   = reader.readU8();
        if (!reader.ok() || !reader.atEnd() || (message.flags & ~kMemoryReadFlagPartial) != 0)
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeMemoryReadEnd(const MemoryReadEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.bytesDelivered);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<MemoryReadEnd> decodeMemoryReadEnd(std::span<const std::byte> payload)
    {
        ByteReader    reader {payload};
        MemoryReadEnd message;
        message.status         = reader.readU8();
        message.bytesDelivered = reader.readU64();
        message.error          = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeMemoryWriteRequest(const MemoryWriteRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeU64(message.address);
        writer.writeBytes(message.data);
        return writer.take();
    }

    std::optional<MemoryWriteRequest> decodeMemoryWriteRequest(std::span<const std::byte> payload)
    {
        ByteReader         reader {payload};
        MemoryWriteRequest message;
        message.pid     = reader.readU32();
        message.address = reader.readU64();
        message.data    = reader.readBytes();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeMemoryWriteResponse(const MemoryWriteResponse& message)
    {
        ByteWriter writer;
        writer.writeU32(message.bytesWritten);
        return writer.take();
    }

    std::optional<MemoryWriteResponse> decodeMemoryWriteResponse(std::span<const std::byte> payload)
    {
        ByteReader          reader {payload};
        MemoryWriteResponse message;
        message.bytesWritten = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeScatterCreateRequest(const ScatterCreateRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        return writer.take();
    }

    std::optional<ScatterCreateRequest> decodeScatterCreateRequest(std::span<const std::byte> payload)
    {
        ByteReader           reader {payload};
        ScatterCreateRequest message;
        message.pid = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeScatterCreateResponse(const ScatterCreateResponse& message)
    {
        ByteWriter writer;
        writer.writeU32(message.scatterId);
        return writer.take();
    }

    std::optional<ScatterCreateResponse> decodeScatterCreateResponse(std::span<const std::byte> payload)
    {
        ByteReader            reader {payload};
        ScatterCreateResponse message;
        message.scatterId = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeScatterPrepareRequest(const ScatterPrepareRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.scatterId);
        writer.writeU32(static_cast<std::uint32_t>(message.entries.size()));
        for (const auto& entry : message.entries)
        {
            writeScatterEntry(writer, entry);
        }
        return writer.take();
    }

    std::optional<ScatterPrepareRequest> decodeScatterPrepareRequest(std::span<const std::byte> payload)
    {
        ByteReader            reader {payload};
        ScatterPrepareRequest message;
        message.scatterId = reader.readU32();
        const auto count  = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        message.entries.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            ScatterEntry entry;
            if (!readScatterEntry(reader, entry))
            {
                return std::nullopt;
            }
            message.entries.push_back(std::move(entry));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeScatterPrepareResponse(const ScatterPrepareResponse& message)
    {
        ByteWriter writer;
        writer.writeU32(message.entryCount);
        return writer.take();
    }

    std::optional<ScatterPrepareResponse> decodeScatterPrepareResponse(std::span<const std::byte> payload)
    {
        ByteReader             reader {payload};
        ScatterPrepareResponse message;
        message.entryCount = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeScatterExecuteRequest(const ScatterExecuteRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.scatterId);
        return writer.take();
    }

    std::optional<ScatterExecuteRequest> decodeScatterExecuteRequest(std::span<const std::byte> payload)
    {
        ByteReader            reader {payload};
        ScatterExecuteRequest message;
        message.scatterId = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeScatterExecuteData(const ScatterExecuteData& message)
    {
        ByteWriter writer;
        writer.writeU32(message.entryIndex);
        writer.writeBytes(message.data);
        return writer.take();
    }

    std::optional<ScatterExecuteData> decodeScatterExecuteData(std::span<const std::byte> payload)
    {
        ByteReader         reader {payload};
        ScatterExecuteData message;
        message.entryIndex = reader.readU32();
        message.data       = reader.readBytes();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeScatterExecuteEnd(const ScatterExecuteEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU32(static_cast<std::uint32_t>(message.outcomes.size()));
        for (const auto& outcome : message.outcomes)
        {
            writeScatterOutcome(writer, outcome);
        }
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<ScatterExecuteEnd> decodeScatterExecuteEnd(std::span<const std::byte> payload)
    {
        ByteReader        reader {payload};
        ScatterExecuteEnd message;
        message.status   = reader.readU8();
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        message.outcomes.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            ScatterOutcome outcome;
            if (!readScatterOutcome(reader, outcome))
            {
                return std::nullopt;
            }
            message.outcomes.push_back(outcome);
        }
        message.error = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeScatterCloseRequest(const ScatterCloseRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.scatterId);
        return writer.take();
    }

    std::optional<ScatterCloseRequest> decodeScatterCloseRequest(std::span<const std::byte> payload)
    {
        ByteReader          reader {payload};
        ScatterCloseRequest message;
        message.scatterId = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeScatterCloseResponse(const ScatterCloseResponse& message)
    {
        ByteWriter writer;
        writer.writeU32(message.scatterId);
        return writer.take();
    }

    std::optional<ScatterCloseResponse> decodeScatterCloseResponse(std::span<const std::byte> payload)
    {
        ByteReader           reader {payload};
        ScatterCloseResponse message;
        message.scatterId = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeScatterExecuteReadRequest(const ScatterExecuteReadRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.scatterId);
        return writer.take();
    }

    std::optional<ScatterExecuteReadRequest> decodeScatterExecuteReadRequest(std::span<const std::byte> payload)
    {
        ByteReader                reader {payload};
        ScatterExecuteReadRequest message;
        message.scatterId = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeScatterExecuteReadData(const ScatterExecuteReadData& message)
    {
        return encodeScatterExecuteData(message);
    }

    std::optional<ScatterExecuteReadData> decodeScatterExecuteReadData(std::span<const std::byte> payload)
    {
        return decodeScatterExecuteData(payload);
    }

    std::vector<std::byte> encodeScatterExecuteReadEnd(const ScatterExecuteReadEnd& message)
    {
        return encodeScatterExecuteEnd(message);
    }

    std::optional<ScatterExecuteReadEnd> decodeScatterExecuteReadEnd(std::span<const std::byte> payload)
    {
        return decodeScatterExecuteEnd(payload);
    }

    std::vector<std::byte> encodeScatterClearRequest(const ScatterClearRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.scatterId);
        return writer.take();
    }

    std::optional<ScatterClearRequest> decodeScatterClearRequest(std::span<const std::byte> payload)
    {
        ByteReader          reader {payload};
        ScatterClearRequest message;
        message.scatterId = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeScatterClearResponse(const ScatterClearResponse& message)
    {
        ByteWriter writer;
        writer.writeU32(message.scatterId);
        return writer.take();
    }

    std::optional<ScatterClearResponse> decodeScatterClearResponse(std::span<const std::byte> payload)
    {
        ByteReader           reader {payload};
        ScatterClearResponse message;
        message.scatterId = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeVirtualQueryRequest(const VirtualQueryRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeU64(message.address);
        return writer.take();
    }

    std::optional<VirtualQueryRequest> decodeVirtualQueryRequest(std::span<const std::byte> payload)
    {
        ByteReader          reader {payload};
        VirtualQueryRequest message;
        message.pid     = reader.readU32();
        message.address = reader.readU64();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeVirtualQueryResponse(const VirtualQueryResponse& message)
    {
        ByteWriter writer;
        writer.writeU64(message.region.baseAddress);
        writer.writeU64(message.region.allocationBase);
        writer.writeU64(message.region.regionSize);
        writer.writeU32(message.region.allocationProtect);
        writer.writeU32(message.region.state);
        writer.writeU32(message.region.protect);
        writer.writeU32(message.region.type);
        writer.writeU32(message.region.partitionId);
        return writer.take();
    }

    std::optional<VirtualQueryResponse> decodeVirtualQueryResponse(std::span<const std::byte> payload)
    {
        ByteReader           reader {payload};
        VirtualQueryResponse message;
        message.region.baseAddress       = reader.readU64();
        message.region.allocationBase    = reader.readU64();
        message.region.regionSize        = reader.readU64();
        message.region.allocationProtect = reader.readU32();
        message.region.state             = reader.readU32();
        message.region.protect           = reader.readU32();
        message.region.type              = reader.readU32();
        message.region.partitionId       = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePageReadRequest(const PageReadRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writePageRanges(writer, message.ranges);
        return writer.take();
    }

    std::optional<PageReadRequest> decodePageReadRequest(std::span<const std::byte> payload)
    {
        ByteReader      reader {payload};
        PageReadRequest message;
        message.pid = reader.readU32();
        if (!readPageRanges(reader, message.ranges))
        {
            return std::nullopt;
        }
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePageReadData(const PageReadData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.pages.size()));
        for (const auto& page : message.pages)
        {
            writer.writeU64(page.address);
            writer.writeU8(page.ok ? 1 : 0);
            writer.writeBytes(page.data);
        }
        return writer.take();
    }

    std::optional<PageReadData> decodePageReadData(std::span<const std::byte> payload)
    {
        ByteReader   reader {payload};
        PageReadData message;
        const auto   count = reader.readU32();
        if (!reader.ok() || count > kMaxPageCount)
        {
            return std::nullopt;
        }
        message.pages.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            PageReadItem page;
            page.address = reader.readU64();
            page.ok      = reader.readU8() != 0;
            page.data    = reader.readBytes();
            if (!reader.ok())
            {
                return std::nullopt;
            }
            message.pages.push_back(std::move(page));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePageReadEnd(const PageReadEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU32(message.pageCount);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<PageReadEnd> decodePageReadEnd(std::span<const std::byte> payload)
    {
        ByteReader  reader {payload};
        PageReadEnd message;
        message.status    = reader.readU8();
        message.pageCount = reader.readU32();
        message.error     = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePrefetchRequest(const PrefetchRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writePageRanges(writer, message.ranges);
        return writer.take();
    }

    std::optional<PrefetchRequest> decodePrefetchRequest(std::span<const std::byte> payload)
    {
        ByteReader      reader {payload};
        PrefetchRequest message;
        message.pid = reader.readU32();
        if (!readPageRanges(reader, message.ranges))
        {
            return std::nullopt;
        }
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePrefetchResponse(const PrefetchResponse& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pageCount);
        return writer.take();
    }

    std::optional<PrefetchResponse> decodePrefetchResponse(std::span<const std::byte> payload)
    {
        ByteReader       reader {payload};
        PrefetchResponse message;
        message.pageCount = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeMemorySearchRequest(const MemorySearchRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeBytes(message.term.pattern);
        writer.writeBytes(message.term.mask);
        writer.writeU32(message.alignBytes);
        writer.writeU64(message.minAddress);
        writer.writeU64(message.maxAddress);
        writer.writeU32(message.maxResults);
        return writer.take();
    }

    std::optional<MemorySearchRequest> decodeMemorySearchRequest(std::span<const std::byte> payload)
    {
        ByteReader          reader {payload};
        MemorySearchRequest message;
        message.pid          = reader.readU32();
        message.term.pattern = reader.readBytes();
        message.term.mask    = reader.readBytes();
        message.alignBytes   = reader.readU32();
        message.minAddress   = reader.readU64();
        message.maxAddress   = reader.readU64();
        message.maxResults   = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        if (message.term.pattern.empty() || message.term.pattern.size() > kMaxSearchPatternLength
            || message.term.mask.size() != message.term.pattern.size())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeMemorySearchData(const MemorySearchData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.addresses.size()));
        for (const auto address : message.addresses)
        {
            writer.writeU64(address);
        }
        return writer.take();
    }

    std::optional<MemorySearchData> decodeMemorySearchData(std::span<const std::byte> payload)
    {
        ByteReader       reader {payload};
        MemorySearchData message;
        const auto       count = reader.readU32();
        if (!reader.ok() || count > kMaxSearchHits || count > payload.size() / sizeof(std::uint64_t))
        {
            return std::nullopt;
        }
        message.addresses.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            message.addresses.push_back(reader.readU64());
        }
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeMemorySearchEnd(const MemorySearchEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU32(message.hitCount);
        writer.writeU8(message.truncated);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<MemorySearchEnd> decodeMemorySearchEnd(std::span<const std::byte> payload)
    {
        ByteReader      reader {payload};
        MemorySearchEnd message;
        message.status    = reader.readU8();
        message.hitCount  = reader.readU32();
        message.truncated = reader.readU8();
        message.error     = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

} // namespace mpfshost
