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
            writer.writeU8(status.stubOwned ? 1 : 0);
            writer.writeU8(status.foreignStub ? 1 : 0);
            writer.writeString(status.architecture);
            return writer.take();
        }

        std::optional<DebugStatus> decodeDebugStatus(std::span<const std::byte> payload)
        {
            ByteReader  reader {payload};
            DebugStatus status;
            status.runState     = static_cast<DebugRunState>(reader.readU8());
            status.stubActive   = reader.readU8() != 0;
            status.hostPaused   = reader.readU8() != 0;
            status.vcpuCount    = reader.readU32();
            status.watchCount   = reader.readU32();
            status.stubOwned    = reader.readU8() != 0;
            status.foreignStub  = reader.readU8() != 0;
            status.architecture = reader.readString();
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

    std::string_view debugWatchStatusName(DebugWatchStatus status) noexcept
    {
        switch (status)
        {
        case DebugWatchStatus::Completed:
            return "completed";
        case DebugWatchStatus::Timeout:
            return "timeout";
        case DebugWatchStatus::Cancelled:
            return "cancelled";
        case DebugWatchStatus::Error:
            return "error";
        }
        return "error";
    }

    std::vector<std::byte> encodeDebugWatchRequest(const DebugWatchRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeU64(message.address);
        writer.writeU32(message.width);
        writer.writeU8(static_cast<std::uint8_t>(message.access));
        writer.writeU32(message.maxHits);
        writer.writeU32(message.timeoutMs);
        return writer.take();
    }

    std::optional<DebugWatchRequest> decodeDebugWatchRequest(std::span<const std::byte> payload)
    {
        ByteReader        reader {payload};
        DebugWatchRequest message;
        message.pid       = reader.readU32();
        message.address   = reader.readU64();
        message.width     = reader.readU32();
        message.access    = static_cast<DebugWatchAccess>(reader.readU8());
        message.maxHits   = reader.readU32();
        message.timeoutMs = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        if (message.access != DebugWatchAccess::Write)
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeDebugWatchData(const DebugWatchData& message)
    {
        ByteWriter writer;
        writer.writeU32(message.hitIndex);
        writer.writeU32(message.vcpu);
        writer.writeString(message.threadId);
        writer.writeU64(message.rip);
        writer.writeU64(message.rsp);
        writer.writeU64(message.cr3);
        writer.writeU64(message.address);
        writer.writeU64(message.oldValue);
        writer.writeU64(message.newValue);
        writer.writeBytes(message.bytesBefore);
        writer.writeString(message.module);
        writer.writeU64(message.moduleOffset);
        writer.writeU8(message.exportFound ? 1 : 0);
        writer.writeU64(message.exportAddress);
        writer.writeU64(message.exportOffset);
        writer.writeString(message.exportName);
        writer.writeU8(message.pdbFound ? 1 : 0);
        writer.writeString(message.pdbSymbol);
        writer.writeU32(message.pdbDisplacement);
        return writer.take();
    }

    std::optional<DebugWatchData> decodeDebugWatchData(std::span<const std::byte> payload)
    {
        ByteReader     reader {payload};
        DebugWatchData message;
        message.hitIndex        = reader.readU32();
        message.vcpu            = reader.readU32();
        message.threadId        = reader.readString();
        message.rip             = reader.readU64();
        message.rsp             = reader.readU64();
        message.cr3             = reader.readU64();
        message.address         = reader.readU64();
        message.oldValue        = reader.readU64();
        message.newValue        = reader.readU64();
        message.bytesBefore     = reader.readBytes();
        message.module          = reader.readString();
        message.moduleOffset    = reader.readU64();
        message.exportFound     = reader.readU8() != 0;
        message.exportAddress   = reader.readU64();
        message.exportOffset    = reader.readU64();
        message.exportName      = reader.readString();
        message.pdbFound        = reader.readU8() != 0;
        message.pdbSymbol       = reader.readString();
        message.pdbDisplacement = reader.readU32();
        if (!reader.ok() || !reader.atEnd() || message.bytesBefore.size() > kDebugWatchBytesBefore)
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeDebugWatchEnd(const DebugWatchEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(static_cast<std::uint8_t>(message.status));
        writer.writeU32(message.hitCount);
        writer.writeU32(message.skippedCount);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<DebugWatchEnd> decodeDebugWatchEnd(std::span<const std::byte> payload)
    {
        ByteReader    reader {payload};
        DebugWatchEnd message;
        message.status       = static_cast<DebugWatchStatus>(reader.readU8());
        message.hitCount     = reader.readU32();
        message.skippedCount = reader.readU32();
        message.error        = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        if (message.status != DebugWatchStatus::Completed && message.status != DebugWatchStatus::Timeout
            && message.status != DebugWatchStatus::Cancelled && message.status != DebugWatchStatus::Error)
        {
            return std::nullopt;
        }
        return message;
    }

    std::string_view debugBreakpointStatusName(DebugBreakpointStatus status) noexcept
    {
        switch (status)
        {
        case DebugBreakpointStatus::Hit:
            return "hit";
        case DebugBreakpointStatus::Timeout:
            return "timeout";
        case DebugBreakpointStatus::Cancelled:
            return "cancelled";
        case DebugBreakpointStatus::Error:
            return "error";
        }
        return "error";
    }

    std::vector<std::byte> encodeDebugBreakpointRequest(const DebugBreakpointRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeU64(message.address);
        writer.writeU8(static_cast<std::uint8_t>(message.kind));
        writer.writeU32(message.timeoutMs);
        return writer.take();
    }

    std::optional<DebugBreakpointRequest> decodeDebugBreakpointRequest(std::span<const std::byte> payload)
    {
        ByteReader             reader {payload};
        DebugBreakpointRequest message;
        message.pid       = reader.readU32();
        message.address   = reader.readU64();
        message.kind      = static_cast<DebugBreakpointKind>(reader.readU8());
        message.timeoutMs = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        if (message.kind != DebugBreakpointKind::Software && message.kind != DebugBreakpointKind::Hardware)
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeDebugBreakpointResponse(const DebugBreakpointResponse& message)
    {
        ByteWriter writer;
        writer.writeU8(static_cast<std::uint8_t>(message.status));
        writer.writeU32(message.skippedCount);
        writer.writeString(message.error);
        writer.writeU32(message.vcpu);
        writer.writeString(message.threadId);
        writer.writeU64(message.rip);
        writer.writeU64(message.rsp);
        writer.writeU64(message.cr3);
        writer.writeU64(message.address);
        writer.writeBytes(message.bytesAtRip);
        writer.writeString(message.module);
        writer.writeU64(message.moduleOffset);
        writer.writeU8(message.exportFound ? 1 : 0);
        writer.writeU64(message.exportAddress);
        writer.writeU64(message.exportOffset);
        writer.writeString(message.exportName);
        writer.writeU8(message.pdbFound ? 1 : 0);
        writer.writeString(message.pdbSymbol);
        writer.writeU32(message.pdbDisplacement);
        return writer.take();
    }

    std::optional<DebugBreakpointResponse> decodeDebugBreakpointResponse(std::span<const std::byte> payload)
    {
        ByteReader              reader {payload};
        DebugBreakpointResponse message;
        message.status          = static_cast<DebugBreakpointStatus>(reader.readU8());
        message.skippedCount    = reader.readU32();
        message.error           = reader.readString();
        message.vcpu            = reader.readU32();
        message.threadId        = reader.readString();
        message.rip             = reader.readU64();
        message.rsp             = reader.readU64();
        message.cr3             = reader.readU64();
        message.address         = reader.readU64();
        message.bytesAtRip      = reader.readBytes();
        message.module          = reader.readString();
        message.moduleOffset    = reader.readU64();
        message.exportFound     = reader.readU8() != 0;
        message.exportAddress   = reader.readU64();
        message.exportOffset    = reader.readU64();
        message.exportName      = reader.readString();
        message.pdbFound        = reader.readU8() != 0;
        message.pdbSymbol       = reader.readString();
        message.pdbDisplacement = reader.readU32();
        if (!reader.ok() || !reader.atEnd() || message.bytesAtRip.size() > kDebugBreakpointBytes)
        {
            return std::nullopt;
        }
        if (message.status != DebugBreakpointStatus::Hit && message.status != DebugBreakpointStatus::Timeout
            && message.status != DebugBreakpointStatus::Cancelled && message.status != DebugBreakpointStatus::Error)
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeDebugStepRequest(const DebugStepRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.vcpu);
        return writer.take();
    }

    std::optional<DebugStepRequest> decodeDebugStepRequest(std::span<const std::byte> payload)
    {
        ByteReader       reader {payload};
        DebugStepRequest message;
        message.vcpu = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeDebugStepResponse(const DebugStepResponse& message)
    {
        ByteWriter writer;
        writeDebugRecord(writer, message.record);
        return writer.take();
    }

    std::optional<DebugStepResponse> decodeDebugStepResponse(std::span<const std::byte> payload)
    {
        ByteReader        reader {payload};
        DebugStepResponse message;
        message.record = readDebugRecord(reader);
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeDebugDetachRequest(const DebugDetachRequest&)
    {
        return {};
    }

    std::optional<DebugDetachRequest> decodeDebugDetachRequest(std::span<const std::byte> payload)
    {
        if (!payload.empty())
        {
            return std::nullopt;
        }
        return DebugDetachRequest {};
    }

    std::vector<std::byte> encodeDebugDetachResponse(const DebugDetachResponse& message)
    {
        ByteWriter writer;
        writer.writeU8(static_cast<std::uint8_t>(message.status.runState));
        writer.writeU8(message.status.stubActive ? 1 : 0);
        writer.writeU8(message.status.hostPaused ? 1 : 0);
        writer.writeU32(message.status.vcpuCount);
        writer.writeU32(message.status.watchCount);
        writer.writeU8(message.status.stubOwned ? 1 : 0);
        writer.writeU8(message.status.foreignStub ? 1 : 0);
        writer.writeU8(message.stubStopped ? 1 : 0);
        writer.writeU8(message.foreignStub ? 1 : 0);
        writer.writeU8(message.guestResumed ? 1 : 0);
        return writer.take();
    }

    std::optional<DebugDetachResponse> decodeDebugDetachResponse(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};

        DebugDetachResponse message;
        message.status.runState    = static_cast<DebugRunState>(reader.readU8());
        message.status.stubActive  = reader.readU8() != 0;
        message.status.hostPaused  = reader.readU8() != 0;
        message.status.vcpuCount   = reader.readU32();
        message.status.watchCount  = reader.readU32();
        message.status.stubOwned   = reader.readU8() != 0;
        message.status.foreignStub = reader.readU8() != 0;
        message.stubStopped        = reader.readU8() != 0;
        message.foreignStub        = reader.readU8() != 0;
        message.guestResumed       = reader.readU8() != 0;
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }
} // namespace mpfshost
