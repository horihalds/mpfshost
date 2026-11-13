#include "core/ipc/message.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace mpfshost
{
    namespace
    {
        bool isKeyNameCharacter(char character)
        {
            return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_';
        }

        bool validKeyName(std::string_view name)
        {
            if (name.empty() || name.size() > kMaxInputKeyNameSize)
            {
                return false;
            }
            for (const char character : name)
            {
                if (!isKeyNameCharacter(character))
                {
                    return false;
                }
            }
            return true;
        }

        bool validButton(std::uint8_t value)
        {
            return value <= static_cast<std::uint8_t>(InputMouseButton::Right);
        }
    } // namespace

    std::vector<std::byte> encodeInputEventRequest(const InputEventRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.events.size()));
        for (const auto& event : message.events)
        {
            writer.writeU8(static_cast<std::uint8_t>(event.kind));
            switch (event.kind)
            {
            case InputEventKind::Key:
                writer.writeU8(event.down ? 1 : 0);
                writer.writeString(event.key);
                break;
            case InputEventKind::PointerMove:
                writer.writeU8(event.absolute ? 1 : 0);
                writer.writeU32(static_cast<std::uint32_t>(event.x));
                writer.writeU32(static_cast<std::uint32_t>(event.y));
                break;
            case InputEventKind::PointerButton:
                writer.writeU8(event.down ? 1 : 0);
                writer.writeU8(static_cast<std::uint8_t>(event.button));
                break;
            }
        }
        return writer.take();
    }

    std::optional<InputEventRequest> decodeInputEventRequest(std::span<const std::byte> payload)
    {
        ByteReader        reader {payload};
        InputEventRequest message;
        const auto        count = reader.readU32();
        if (!reader.ok() || count > kMaxInputEvents || count > payload.size())
        {
            return std::nullopt;
        }

        message.events.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            InputEvent event;
            const auto kind = reader.readU8();
            if (!reader.ok())
            {
                return std::nullopt;
            }

            switch (static_cast<InputEventKind>(kind))
            {
            case InputEventKind::Key:
                event.kind = InputEventKind::Key;
                event.down = reader.readU8() != 0;
                event.key  = reader.readString();
                if (!reader.ok() || !validKeyName(event.key))
                {
                    return std::nullopt;
                }
                break;
            case InputEventKind::PointerMove:
                event.kind     = InputEventKind::PointerMove;
                event.absolute = reader.readU8() != 0;
                event.x        = static_cast<std::int32_t>(reader.readU32());
                event.y        = static_cast<std::int32_t>(reader.readU32());
                if (!reader.ok())
                {
                    return std::nullopt;
                }
                if (event.absolute)
                {
                    if (event.x < 0 || event.x > kInputAbsoluteMax || event.y < 0 || event.y > kInputAbsoluteMax)
                    {
                        return std::nullopt;
                    }
                }
                break;
            case InputEventKind::PointerButton:
                event.kind   = InputEventKind::PointerButton;
                event.down   = reader.readU8() != 0;
                event.button = static_cast<InputMouseButton>(reader.readU8());
                if (!reader.ok() || !validButton(static_cast<std::uint8_t>(event.button)))
                {
                    return std::nullopt;
                }
                break;
            default:
                return std::nullopt;
            }

            message.events.push_back(std::move(event));
        }

        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeInputEventResponse(const InputEventResponse& message)
    {
        ByteWriter writer;
        writer.writeU32(message.eventsSent);
        return writer.take();
    }

    std::optional<InputEventResponse> decodeInputEventResponse(std::span<const std::byte> payload)
    {
        ByteReader         reader {payload};
        InputEventResponse message;
        message.eventsSent = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }
} // namespace mpfshost
