#include "core/ipc/message.hpp"
#include "core/ipc/message_codec.hpp"

namespace mpfshost
{
    using namespace detail;

    std::vector<std::byte> encodeListTargetsResponse(const ListTargetsResponse& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.targets.size()));
        for (const auto& target : message.targets)
        {
            writeTarget(writer, target);
        }
        return writer.take();
    }

    std::optional<ListTargetsResponse> decodeListTargetsResponse(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        ListTargetsResponse message;
        message.targets.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            TargetDescriptor target;
            if (!readTarget(reader, target))
            {
                return std::nullopt;
            }
            message.targets.push_back(std::move(target));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeCapabilitiesResponse(const CapabilitiesResponse& message)
    {
        ByteWriter writer;
        writer.writeString(message.serverVersion);
        writer.writeU32(static_cast<std::uint32_t>(message.requestTypes.size()));
        for (const auto type : message.requestTypes)
        {
            writer.writeU16(type);
        }
        return writer.take();
    }

    std::optional<CapabilitiesResponse> decodeCapabilitiesResponse(std::span<const std::byte> payload)
    {
        ByteReader           reader {payload};
        CapabilitiesResponse message;
        message.serverVersion = reader.readString();
        const auto count      = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        message.requestTypes.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            message.requestTypes.push_back(reader.readU16());
        }
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeBindRequest(const BindRequest& message)
    {
        ByteWriter writer;
        writer.writeString(message.targetName);
        return writer.take();
    }

    std::optional<BindRequest> decodeBindRequest(std::span<const std::byte> payload)
    {
        ByteReader  reader {payload};
        BindRequest message;
        message.targetName = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeBindResponse(const BindResponse& message)
    {
        ByteWriter writer;
        writer.writeU64(message.sessionId);
        writeTarget(writer, message.target);
        return writer.take();
    }

    std::optional<BindResponse> decodeBindResponse(std::span<const std::byte> payload)
    {
        ByteReader   reader {payload};
        BindResponse message;
        message.sessionId = reader.readU64();
        if (!readTarget(reader, message.target) || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeErrorResponse(const ErrorResponse& message)
    {
        ByteWriter writer;
        writer.writeU16(static_cast<std::uint16_t>(message.code));
        writer.writeString(message.message);
        return writer.take();
    }

    std::optional<ErrorResponse> decodeErrorResponse(std::span<const std::byte> payload)
    {
        ByteReader    reader {payload};
        ErrorResponse message;
        message.code    = static_cast<ErrorCode>(reader.readU16());
        message.message = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeCancelRequest(const CancelRequest& message)
    {
        ByteWriter writer;
        writer.writeU64(message.requestId);
        return writer.take();
    }

    std::optional<CancelRequest> decodeCancelRequest(std::span<const std::byte> payload)
    {
        ByteReader    reader {payload};
        CancelRequest message;
        message.requestId = reader.readU64();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeCancelResponse(const CancelResponse& message)
    {
        ByteWriter writer;
        writer.writeU8(message.cancelled);
        writer.writeU64(message.requestId);
        return writer.take();
    }

    std::optional<CancelResponse> decodeCancelResponse(std::span<const std::byte> payload)
    {
        ByteReader     reader {payload};
        CancelResponse message;
        message.cancelled = reader.readU8();
        message.requestId = reader.readU64();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

} // namespace mpfshost
