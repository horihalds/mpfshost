#include "core/ipc/message.hpp"
#include "core/ipc/message_codec.hpp"

namespace mpfshost
{
    using namespace detail;

    std::vector<std::byte> encodeVfsListRequest(const VfsListRequest& message)
    {
        ByteWriter writer;
        writer.writeString(message.path);
        writer.writeString(message.nameFilter);
        return writer.take();
    }

    std::optional<VfsListRequest> decodeVfsListRequest(std::span<const std::byte> payload)
    {
        ByteReader     reader {payload};
        VfsListRequest message;
        message.path       = reader.readString();
        message.nameFilter = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeVfsListData(const VfsListData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.entries.size()));
        for (const auto& entry : message.entries)
        {
            writeVfsEntry(writer, entry);
        }
        return writer.take();
    }

    std::optional<VfsListData> decodeVfsListData(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        VfsListData message;
        message.entries.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            VfsEntry entry;
            if (!readVfsEntry(reader, entry))
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

    std::vector<std::byte> encodeVfsListEnd(const VfsListEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.entryCount);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<VfsListEnd> decodeVfsListEnd(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        VfsListEnd message;
        message.status     = reader.readU8();
        message.entryCount = reader.readU64();
        message.error      = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeVfsReadRequest(const VfsReadRequest& message)
    {
        ByteWriter writer;
        writer.writeString(message.path);
        writer.writeU64(message.offset);
        writer.writeU32(message.size);
        return writer.take();
    }

    std::optional<VfsReadRequest> decodeVfsReadRequest(std::span<const std::byte> payload)
    {
        ByteReader     reader {payload};
        VfsReadRequest message;
        message.path   = reader.readString();
        message.offset = reader.readU64();
        message.size   = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeVfsReadEnd(const VfsReadEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.bytesRead);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<VfsReadEnd> decodeVfsReadEnd(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        VfsReadEnd message;
        message.status    = reader.readU8();
        message.bytesRead = reader.readU64();
        message.error     = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeVfsWriteRequest(const VfsWriteRequest& message)
    {
        ByteWriter writer;
        writer.writeString(message.path);
        writer.writeU64(message.offset);
        writer.writeBytes(message.data);
        return writer.take();
    }

    std::optional<VfsWriteRequest> decodeVfsWriteRequest(std::span<const std::byte> payload)
    {
        ByteReader      reader {payload};
        VfsWriteRequest message;
        message.path   = reader.readString();
        message.offset = reader.readU64();
        message.data   = reader.readBytes();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeVfsWriteResponse(const VfsWriteResponse& message)
    {
        ByteWriter writer;
        writer.writeU32(message.bytesWritten);
        return writer.take();
    }

    std::optional<VfsWriteResponse> decodeVfsWriteResponse(std::span<const std::byte> payload)
    {
        ByteReader       reader {payload};
        VfsWriteResponse message;
        message.bytesWritten = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

} // namespace mpfshost
