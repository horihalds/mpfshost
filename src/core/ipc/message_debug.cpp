#include "core/ipc/message.hpp"

#include <string_view>

namespace mpfshost
{
    namespace
    {
        std::vector<std::byte> encodeDebugStatus(const DebugStatus& status)
        {
            ByteWriter writer;
            writer.writeU8(static_cast<std::uint8_t>(status.runState));
            writer.writeU8(status.stubActive ? 1 : 0);
            writer.writeU8(status.hostPaused ? 1 : 0);
            writer.writeU32(status.vcpuCount);
            writer.writeU32(status.watchCount);
            return writer.take();
        }

        std::optional<DebugStatus> decodeDebugStatus(std::span<const std::byte> payload)
        {
            ByteReader  reader {payload};
            DebugStatus status;
            status.runState   = static_cast<DebugRunState>(reader.readU8());
            status.stubActive = reader.readU8() != 0;
            status.hostPaused = reader.readU8() != 0;
            status.vcpuCount  = reader.readU32();
            status.watchCount = reader.readU32();
            if (!reader.ok() || !reader.atEnd())
            {
                return std::nullopt;
            }
            return status;
        }
    } // namespace

    std::string_view debugRunStateName(DebugRunState state) noexcept
    {
        switch (state)
        {
        case DebugRunState::Unknown:
            return "unknown";
        case DebugRunState::Running:
            return "running";
        case DebugRunState::Paused:
            return "paused";
        case DebugRunState::Shutdown:
            return "shutdown";
        case DebugRunState::Suspended:
            return "suspended";
        case DebugRunState::GuestPanicked:
            return "guest-panicked";
        case DebugRunState::Other:
            return "other";
        }
        return "unknown";
    }

    std::vector<std::byte> encodeDebugControlRequest(const DebugControlRequest& message)
    {
        ByteWriter writer;
        writer.writeU8(static_cast<std::uint8_t>(message.action));
        return writer.take();
    }

    std::optional<DebugControlRequest> decodeDebugControlRequest(std::span<const std::byte> payload)
    {
        ByteReader          reader {payload};
        DebugControlRequest message;
        message.action = static_cast<DebugControlAction>(reader.readU8());
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        if (message.action != DebugControlAction::Stop && message.action != DebugControlAction::Continue)
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeDebugStateResponse(const DebugStateResponse& message)
    {
        return encodeDebugStatus(message);
    }

    std::optional<DebugStateResponse> decodeDebugStateResponse(std::span<const std::byte> payload)
    {
        return decodeDebugStatus(payload);
    }

    std::vector<std::byte> encodeDebugControlResponse(const DebugControlResponse& message)
    {
        return encodeDebugStatus(message);
    }

    std::optional<DebugControlResponse> decodeDebugControlResponse(std::span<const std::byte> payload)
    {
        return decodeDebugStatus(payload);
    }
} // namespace mpfshost
