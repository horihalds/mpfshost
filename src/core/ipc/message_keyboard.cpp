#include "core/ipc/message.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace mpfshost
{
    std::vector<std::byte> encodeKeyboardStateRequest(const KeyboardStateRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.sessionId);
        return writer.take();
    }

    std::optional<KeyboardStateRequest> decodeKeyboardStateRequest(std::span<const std::byte> payload)
    {
        ByteReader           reader {payload};
        KeyboardStateRequest message;
        message.sessionId = reader.readU32();
        if (!reader.ok() || !reader.atEnd() || message.sessionId > kMaxKeyboardSessions)
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeKeyboardStateResponse(const KeyboardStateResponse& message)
    {
        ByteWriter writer;
        writer.writeU32(message.sessionId);
        writer.writeU32(message.proxyPid);
        writer.writeU32(message.windowsBuild);
        writer.writeU64(message.keyStateAddress);
        writer.writeU64(message.moduleBase);
        writer.writeU64(message.keyStateOffset);
        writer.writeString(message.moduleName);
        writer.writeBytes(message.state);
        writer.writeU32(static_cast<std::uint32_t>(message.pressedKeys.size()));
        for (const auto virtualKey : message.pressedKeys)
        {
            writer.writeU8(virtualKey);
        }
        return writer.take();
    }

    std::optional<KeyboardStateResponse> decodeKeyboardStateResponse(std::span<const std::byte> payload)
    {
        ByteReader            reader {payload};
        KeyboardStateResponse message;
        message.sessionId       = reader.readU32();
        message.proxyPid        = reader.readU32();
        message.windowsBuild    = reader.readU32();
        message.keyStateAddress = reader.readU64();
        message.moduleBase      = reader.readU64();
        message.keyStateOffset  = reader.readU64();
        message.moduleName      = reader.readString();
        message.state           = reader.readBytes();

        const auto count = reader.readU32();
        if (!reader.ok() || count > kKeyboardVirtualKeyCount || count > payload.size())
        {
            return std::nullopt;
        }
        message.pressedKeys.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            message.pressedKeys.push_back(reader.readU8());
        }
        if (!reader.ok() || !reader.atEnd() || message.state.size() != kKeyboardStateBytes)
        {
            return std::nullopt;
        }
        return message;
    }
} // namespace mpfshost
