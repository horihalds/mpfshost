#include "core/ipc/message.hpp"

namespace mpfshost
{
    std::vector<std::byte> encodeDisassembleRequest(const DisassembleRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeU64(message.address);
        writer.writeU32(message.count);
        return writer.take();
    }

    std::optional<DisassembleRequest> decodeDisassembleRequest(std::span<const std::byte> payload)
    {
        ByteReader         reader {payload};
        DisassembleRequest message;
        message.pid     = reader.readU32();
        message.address = reader.readU64();
        message.count   = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        if (message.count == 0 || message.count > kMaxDisassembleInstructions)
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeDisassembleResponse(const DisassembleResponse& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.instructions.size()));
        for (const auto& instruction : message.instructions)
        {
            writer.writeU64(instruction.address);
            writer.writeBytes(instruction.bytes);
            writer.writeString(instruction.text);
        }
        return writer.take();
    }

    std::optional<DisassembleResponse> decodeDisassembleResponse(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size() || count > kMaxDisassembleInstructions)
        {
            return std::nullopt;
        }

        DisassembleResponse message;
        message.instructions.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            InstructionRecord instruction;
            instruction.address = reader.readU64();
            instruction.bytes   = reader.readBytes();
            instruction.text    = reader.readString();
            if (!reader.ok() || instruction.bytes.size() > 15)
            {
                return std::nullopt;
            }
            message.instructions.push_back(std::move(instruction));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }
} // namespace mpfshost
