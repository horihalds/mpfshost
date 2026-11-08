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

    namespace
    {
        void writeDebugRecord(ByteWriter& writer, const DebugRegisterRecord& record)
        {
            writer.writeU32(record.vcpu);
            writer.writeString(record.threadId);
            writer.writeU64(record.rip);
            writer.writeU64(record.rsp);
            writer.writeU64(record.rbp);
            writer.writeU64(record.eflags);
            writer.writeU64(record.cr0);
            writer.writeU64(record.cr3);
            writer.writeU64(record.cr4);
            writer.writeU64(record.cr8);
            writer.writeU64(record.rax);
            writer.writeU64(record.rbx);
            writer.writeU64(record.rcx);
            writer.writeU64(record.rdx);
            writer.writeU64(record.rsi);
            writer.writeU64(record.rdi);
            writer.writeU64(record.r8);
            writer.writeU64(record.r9);
            writer.writeU64(record.r10);
            writer.writeU64(record.r11);
            writer.writeU64(record.r12);
            writer.writeU64(record.r13);
            writer.writeU64(record.r14);
            writer.writeU64(record.r15);
        }

        DebugRegisterRecord readDebugRecord(ByteReader& reader)
        {
            DebugRegisterRecord record;
            record.vcpu     = reader.readU32();
            record.threadId = reader.readString();
            record.rip      = reader.readU64();
            record.rsp      = reader.readU64();
            record.rbp      = reader.readU64();
            record.eflags   = reader.readU64();
            record.cr0      = reader.readU64();
            record.cr3      = reader.readU64();
            record.cr4      = reader.readU64();
            record.cr8      = reader.readU64();
            record.rax      = reader.readU64();
            record.rbx      = reader.readU64();
            record.rcx      = reader.readU64();
            record.rdx      = reader.readU64();
            record.rsi      = reader.readU64();
            record.rdi      = reader.readU64();
            record.r8       = reader.readU64();
            record.r9       = reader.readU64();
            record.r10      = reader.readU64();
            record.r11      = reader.readU64();
            record.r12      = reader.readU64();
            record.r13      = reader.readU64();
            record.r14      = reader.readU64();
            record.r15      = reader.readU64();
            return record;
        }
    } // namespace

    std::vector<std::byte> encodeDebugRegistersRequest(const DebugRegistersRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.vcpu);
        return writer.take();
    }

    std::optional<DebugRegistersRequest> decodeDebugRegistersRequest(std::span<const std::byte> payload)
    {
        ByteReader            reader {payload};
        DebugRegistersRequest message;
        message.vcpu = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeDebugRegistersResponse(const DebugRegistersResponse& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.records.size()));
        for (const auto& record : message.records)
        {
            writeDebugRecord(writer, record);
        }
        return writer.take();
    }

    std::optional<DebugRegistersResponse> decodeDebugRegistersResponse(std::span<const std::byte> payload)
    {
        ByteReader             reader {payload};
        DebugRegistersResponse message;
        const auto             count = reader.readU32();
        if (!reader.ok() || count > payload.size() || count > kMaxDebugVcpuRecords)
        {
            return std::nullopt;
        }

        message.records.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            message.records.push_back(readDebugRecord(reader));
            if (!reader.ok())
            {
                return std::nullopt;
            }
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }
} // namespace mpfshost
