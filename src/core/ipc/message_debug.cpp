#include "core/ipc/message.hpp"

#include <string_view>

namespace mpfshost
{
    namespace
    {
        void writeDebugStatus(ByteWriter& writer, const DebugStatus& status)
        {
            writer.writeU8(static_cast<std::uint8_t>(status.runState));
            writer.writeU8(status.stubActive ? 1 : 0);
            writer.writeU8(status.hostPaused ? 1 : 0);
            writer.writeU32(status.vcpuCount);
            writer.writeU32(status.watchCount);
            writer.writeU8(status.stubOwned ? 1 : 0);
            writer.writeU8(status.foreignStub ? 1 : 0);
            writer.writeString(status.architecture);
        }

        DebugStatus readDebugStatus(ByteReader& reader)
        {
            DebugStatus status;
            status.runState     = static_cast<DebugRunState>(reader.readU8());
            status.stubActive   = reader.readU8() != 0;
            status.hostPaused   = reader.readU8() != 0;
            status.vcpuCount    = reader.readU32();
            status.watchCount   = reader.readU32();
            status.stubOwned    = reader.readU8() != 0;
            status.foreignStub  = reader.readU8() != 0;
            status.architecture = reader.readString();
            return status;
        }

        std::vector<std::byte> encodeDebugStatus(const DebugStatus& status)
        {
            ByteWriter writer;
            writeDebugStatus(writer, status);
            return writer.take();
        }

        std::optional<DebugStatus> decodeDebugStatus(std::span<const std::byte> payload)
        {
            ByteReader  reader {payload};
            DebugStatus status = readDebugStatus(reader);
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
            writer.writeU32(record.guestPid);
            writer.writeU32(record.guestTid);
            writer.writeU8(record.processFound ? 1 : 0);
            writer.writeU8(record.threadFound ? 1 : 0);
        }

        DebugRegisterRecord readDebugRecord(ByteReader& reader)
        {
            DebugRegisterRecord record;
            record.vcpu         = reader.readU32();
            record.threadId     = reader.readString();
            record.rip          = reader.readU64();
            record.rsp          = reader.readU64();
            record.rbp          = reader.readU64();
            record.eflags       = reader.readU64();
            record.cr0          = reader.readU64();
            record.cr3          = reader.readU64();
            record.cr4          = reader.readU64();
            record.cr8          = reader.readU64();
            record.rax          = reader.readU64();
            record.rbx          = reader.readU64();
            record.rcx          = reader.readU64();
            record.rdx          = reader.readU64();
            record.rsi          = reader.readU64();
            record.rdi          = reader.readU64();
            record.r8           = reader.readU64();
            record.r9           = reader.readU64();
            record.r10          = reader.readU64();
            record.r11          = reader.readU64();
            record.r12          = reader.readU64();
            record.r13          = reader.readU64();
            record.r14          = reader.readU64();
            record.r15          = reader.readU64();
            record.guestPid     = reader.readU32();
            record.guestTid     = reader.readU32();
            record.processFound = reader.readU8() != 0;
            record.threadFound  = reader.readU8() != 0;
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

    std::vector<std::byte> encodeDebugThreadsRequest(const DebugThreadsRequest&)
    {
        return {};
    }

    std::optional<DebugThreadsRequest> decodeDebugThreadsRequest(std::span<const std::byte> payload)
    {
        if (!payload.empty())
        {
            return std::nullopt;
        }
        return DebugThreadsRequest {};
    }

    std::vector<std::byte> encodeDebugThreadsResponse(const DebugThreadsResponse& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.threads.size()));
        for (const auto& thread : message.threads)
        {
            writer.writeU32(thread.vcpu);
            writer.writeString(thread.threadId);
            writer.writeU64(thread.rip);
            writer.writeU64(thread.rsp);
            writer.writeU64(thread.cr3);
            writer.writeU8(thread.processFound ? 1 : 0);
            writer.writeU32(thread.pid);
            writer.writeU8(thread.threadFound ? 1 : 0);
            writer.writeU32(thread.tid);
        }
        return writer.take();
    }

    std::optional<DebugThreadsResponse> decodeDebugThreadsResponse(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size() || count > kMaxDebugVcpuRecords)
        {
            return std::nullopt;
        }

        DebugThreadsResponse message;
        message.threads.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            DebugThreadRecord thread;
            thread.vcpu         = reader.readU32();
            thread.threadId     = reader.readString();
            thread.rip          = reader.readU64();
            thread.rsp          = reader.readU64();
            thread.cr3          = reader.readU64();
            thread.processFound = reader.readU8() != 0;
            thread.pid          = reader.readU32();
            thread.threadFound  = reader.readU8() != 0;
            thread.tid          = reader.readU32();
            if (!reader.ok())
            {
                return std::nullopt;
            }
            message.threads.push_back(std::move(thread));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeDebugDisassembleRequest(const DebugDisassembleRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeU64(message.address);
        writer.writeU32(message.count);
        return writer.take();
    }

    std::optional<DebugDisassembleRequest> decodeDebugDisassembleRequest(std::span<const std::byte> payload)
    {
        ByteReader              reader {payload};
        DebugDisassembleRequest message;
        message.pid     = reader.readU32();
        message.address = reader.readU64();
        message.count   = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        if (message.count == 0 || message.count > kMaxDebugDisassembleInstructions)
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeDebugDisassembleResponse(const DebugDisassembleResponse& message)
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

    std::optional<DebugDisassembleResponse> decodeDebugDisassembleResponse(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size() || count > kMaxDebugDisassembleInstructions)
        {
            return std::nullopt;
        }

        DebugDisassembleResponse message;
        message.instructions.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            DebugInstructionRecord instruction;
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

    std::vector<std::byte> encodeDebugBacktraceRequest(const DebugBacktraceRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeU32(message.vcpu);
        writer.writeString(message.threadId);
        writer.writeU32(message.maxFrames);
        return writer.take();
    }

    std::optional<DebugBacktraceRequest> decodeDebugBacktraceRequest(std::span<const std::byte> payload)
    {
        ByteReader            reader {payload};
        DebugBacktraceRequest message;
        message.pid       = reader.readU32();
        message.vcpu      = reader.readU32();
        message.threadId  = reader.readString();
        message.maxFrames = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        if (message.maxFrames == 0 || message.maxFrames > kMaxDebugBacktraceFrames)
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeDebugBacktraceResponse(const DebugBacktraceResponse& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.frames.size()));
        for (const auto& frame : message.frames)
        {
            writer.writeU64(frame.address);
            writer.writeU64(frame.framePointer);
            writer.writeString(frame.module);
            writer.writeU64(frame.moduleOffset);
            writer.writeU8(frame.exportFound ? 1 : 0);
            writer.writeU64(frame.exportAddress);
            writer.writeU64(frame.exportOffset);
            writer.writeString(frame.exportName);
            writer.writeU8(frame.pdbFound ? 1 : 0);
            writer.writeString(frame.pdbSymbol);
            writer.writeU32(frame.pdbDisplacement);
        }
        writer.writeU8(message.truncated ? 1 : 0);
        writer.writeString(message.stopReason);
        return writer.take();
    }

    std::optional<DebugBacktraceResponse> decodeDebugBacktraceResponse(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size() || count > kMaxDebugBacktraceFrames)
        {
            return std::nullopt;
        }

        DebugBacktraceResponse message;
        message.frames.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            DebugBacktraceFrame frame;
            frame.address         = reader.readU64();
            frame.framePointer    = reader.readU64();
            frame.module          = reader.readString();
            frame.moduleOffset    = reader.readU64();
            frame.exportFound     = reader.readU8() != 0;
            frame.exportAddress   = reader.readU64();
            frame.exportOffset    = reader.readU64();
            frame.exportName      = reader.readString();
            frame.pdbFound        = reader.readU8() != 0;
            frame.pdbSymbol       = reader.readString();
            frame.pdbDisplacement = reader.readU32();
            if (!reader.ok())
            {
                return std::nullopt;
            }
            message.frames.push_back(std::move(frame));
        }
        message.truncated  = reader.readU8() != 0;
        message.stopReason = reader.readString();
        if (!reader.ok() || !reader.atEnd())
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
        writer.writeString(message.module);
        writer.writeString(message.symbol);
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
        message.module    = reader.readString();
        message.symbol    = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        if (message.access != DebugWatchAccess::Write)
        {
            return std::nullopt;
        }
        if (message.module.empty() && !message.symbol.empty())
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
        writer.writeU32(message.guestPid);
        writer.writeU32(message.guestTid);
        writer.writeU8(message.processFound ? 1 : 0);
        writer.writeU8(message.threadFound ? 1 : 0);
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
        message.guestPid        = reader.readU32();
        message.guestTid        = reader.readU32();
        message.processFound    = reader.readU8() != 0;
        message.threadFound     = reader.readU8() != 0;
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
        writer.writeString(message.module);
        writer.writeString(message.symbol);
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
        message.module    = reader.readString();
        message.symbol    = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        if (message.kind != DebugBreakpointKind::Software && message.kind != DebugBreakpointKind::Hardware)
        {
            return std::nullopt;
        }
        if (message.module.empty() && !message.symbol.empty())
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
        writer.writeU32(message.guestPid);
        writer.writeU32(message.guestTid);
        writer.writeU8(message.processFound ? 1 : 0);
        writer.writeU8(message.threadFound ? 1 : 0);
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
        message.guestPid        = reader.readU32();
        message.guestTid        = reader.readU32();
        message.processFound    = reader.readU8() != 0;
        message.threadFound     = reader.readU8() != 0;
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
        writer.writeString(message.threadId);
        return writer.take();
    }

    std::optional<DebugStepRequest> decodeDebugStepRequest(std::span<const std::byte> payload)
    {
        ByteReader       reader {payload};
        DebugStepRequest message;
        message.vcpu     = reader.readU32();
        message.threadId = reader.readString();
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

    std::vector<std::byte> encodeDebugAttachRequest(const DebugAttachRequest&)
    {
        return {};
    }

    std::optional<DebugAttachRequest> decodeDebugAttachRequest(std::span<const std::byte> payload)
    {
        if (!payload.empty())
        {
            return std::nullopt;
        }
        return DebugAttachRequest {};
    }

    std::vector<std::byte> encodeDebugAttachResponse(const DebugAttachResponse& message)
    {
        ByteWriter writer;
        writeDebugStatus(writer, message.status);
        writer.writeString(message.stubMode);
        writer.writeU8(message.guestPaused ? 1 : 0);
        return writer.take();
    }

    std::optional<DebugAttachResponse> decodeDebugAttachResponse(std::span<const std::byte> payload)
    {
        ByteReader          reader {payload};
        DebugAttachResponse message;
        message.status      = readDebugStatus(reader);
        message.stubMode    = reader.readString();
        message.guestPaused = reader.readU8() != 0;
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        if (message.stubMode != "unix" && message.stubMode != "tcp")
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
        writeDebugStatus(writer, message.status);
        writer.writeU8(message.stubStopped ? 1 : 0);
        writer.writeU8(message.foreignStub ? 1 : 0);
        writer.writeU8(message.guestResumed ? 1 : 0);
        writer.writeU8(message.detached ? 1 : 0);
        return writer.take();
    }

    std::optional<DebugDetachResponse> decodeDebugDetachResponse(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};

        DebugDetachResponse message;
        message.status       = readDebugStatus(reader);
        message.stubStopped  = reader.readU8() != 0;
        message.foreignStub  = reader.readU8() != 0;
        message.guestResumed = reader.readU8() != 0;
        message.detached     = reader.readU8() != 0;
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    namespace
    {
        void writeDebugBreakpointArmResponse(ByteWriter& writer, const DebugBreakpointArmResponse& message)
        {
            writer.writeU32(message.id);
            writer.writeU32(message.pid);
            writer.writeU64(message.address);
            writer.writeU8(static_cast<std::uint8_t>(message.kind));
            writer.writeU32(message.slot);
        }

        bool readDebugBreakpointArmResponse(ByteReader& reader, DebugBreakpointArmResponse& message)
        {
            message.id      = reader.readU32();
            message.pid     = reader.readU32();
            message.address = reader.readU64();
            message.kind    = static_cast<DebugBreakpointKind>(reader.readU8());
            message.slot    = reader.readU32();
            return reader.ok()
                && (message.kind == DebugBreakpointKind::Software || message.kind == DebugBreakpointKind::Hardware);
        }

        void writeDebugWatchArmResponse(ByteWriter& writer, const DebugWatchArmResponse& message)
        {
            writer.writeU32(message.id);
            writer.writeU32(message.pid);
            writer.writeU64(message.address);
            writer.writeU32(message.width);
            writer.writeU8(static_cast<std::uint8_t>(message.access));
            writer.writeU32(message.slot);
        }

        bool readDebugWatchArmResponse(ByteReader& reader, DebugWatchArmResponse& message)
        {
            message.id      = reader.readU32();
            message.pid     = reader.readU32();
            message.address = reader.readU64();
            message.width   = reader.readU32();
            message.access  = static_cast<DebugWatchAccess>(reader.readU8());
            message.slot    = reader.readU32();
            return reader.ok() && message.access == DebugWatchAccess::Write;
        }
    } // namespace

    std::vector<std::byte> encodeDebugBreakpointArmRequest(const DebugBreakpointArmRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeU64(message.address);
        writer.writeU8(static_cast<std::uint8_t>(message.kind));
        return writer.take();
    }

    std::optional<DebugBreakpointArmRequest> decodeDebugBreakpointArmRequest(std::span<const std::byte> payload)
    {
        ByteReader                reader {payload};
        DebugBreakpointArmRequest message;
        message.pid     = reader.readU32();
        message.address = reader.readU64();
        message.kind    = static_cast<DebugBreakpointKind>(reader.readU8());
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

    std::vector<std::byte> encodeDebugBreakpointArmResponse(const DebugBreakpointArmResponse& message)
    {
        ByteWriter writer;
        writeDebugBreakpointArmResponse(writer, message);
        return writer.take();
    }

    std::optional<DebugBreakpointArmResponse> decodeDebugBreakpointArmResponse(std::span<const std::byte> payload)
    {
        ByteReader                 reader {payload};
        DebugBreakpointArmResponse message;
        if (!readDebugBreakpointArmResponse(reader, message) || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeDebugBreakpointRemoveRequest(const DebugBreakpointRemoveRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.id);
        return writer.take();
    }

    std::optional<DebugBreakpointRemoveRequest> decodeDebugBreakpointRemoveRequest(std::span<const std::byte> payload)
    {
        ByteReader                   reader {payload};
        DebugBreakpointRemoveRequest message;
        message.id = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeDebugBreakpointListRequest(const DebugBreakpointListRequest&)
    {
        return {};
    }

    std::optional<DebugBreakpointListRequest> decodeDebugBreakpointListRequest(std::span<const std::byte> payload)
    {
        if (!payload.empty())
        {
            return std::nullopt;
        }
        return DebugBreakpointListRequest {};
    }

    std::vector<std::byte> encodeDebugBreakpointListResponse(const DebugBreakpointListResponse& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.breakpoints.size()));
        for (const auto& breakpoint : message.breakpoints)
        {
            writeDebugBreakpointArmResponse(writer, breakpoint);
        }
        return writer.take();
    }

    std::optional<DebugBreakpointListResponse> decodeDebugBreakpointListResponse(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > kMaxDebugArmedResources)
        {
            return std::nullopt;
        }

        DebugBreakpointListResponse message;
        message.breakpoints.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            DebugBreakpointArmResponse breakpoint;
            if (!readDebugBreakpointArmResponse(reader, breakpoint))
            {
                return std::nullopt;
            }
            message.breakpoints.push_back(std::move(breakpoint));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeDebugWatchArmRequest(const DebugWatchArmRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeU64(message.address);
        writer.writeU32(message.width);
        writer.writeU8(static_cast<std::uint8_t>(message.access));
        return writer.take();
    }

    std::optional<DebugWatchArmRequest> decodeDebugWatchArmRequest(std::span<const std::byte> payload)
    {
        ByteReader           reader {payload};
        DebugWatchArmRequest message;
        message.pid     = reader.readU32();
        message.address = reader.readU64();
        message.width   = reader.readU32();
        message.access  = static_cast<DebugWatchAccess>(reader.readU8());
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

    std::vector<std::byte> encodeDebugWatchArmResponse(const DebugWatchArmResponse& message)
    {
        ByteWriter writer;
        writeDebugWatchArmResponse(writer, message);
        return writer.take();
    }

    std::optional<DebugWatchArmResponse> decodeDebugWatchArmResponse(std::span<const std::byte> payload)
    {
        ByteReader            reader {payload};
        DebugWatchArmResponse message;
        if (!readDebugWatchArmResponse(reader, message) || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeDebugWatchRemoveRequest(const DebugWatchRemoveRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.id);
        return writer.take();
    }

    std::optional<DebugWatchRemoveRequest> decodeDebugWatchRemoveRequest(std::span<const std::byte> payload)
    {
        ByteReader              reader {payload};
        DebugWatchRemoveRequest message;
        message.id = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeDebugWatchListRequest(const DebugWatchListRequest&)
    {
        return {};
    }

    std::optional<DebugWatchListRequest> decodeDebugWatchListRequest(std::span<const std::byte> payload)
    {
        if (!payload.empty())
        {
            return std::nullopt;
        }
        return DebugWatchListRequest {};
    }

    std::vector<std::byte> encodeDebugWatchListResponse(const DebugWatchListResponse& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.watches.size()));
        for (const auto& watch : message.watches)
        {
            writeDebugWatchArmResponse(writer, watch);
        }
        return writer.take();
    }

    std::optional<DebugWatchListResponse> decodeDebugWatchListResponse(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > kMaxDebugArmedResources)
        {
            return std::nullopt;
        }

        DebugWatchListResponse message;
        message.watches.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            DebugWatchArmResponse watch;
            if (!readDebugWatchArmResponse(reader, watch))
            {
                return std::nullopt;
            }
            message.watches.push_back(std::move(watch));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeDebugResourceRemoveResponse(const DebugResourceRemoveResponse& message)
    {
        ByteWriter writer;
        writer.writeU32(message.id);
        writer.writeU8(message.removed ? 1 : 0);
        return writer.take();
    }

    std::optional<DebugResourceRemoveResponse> decodeDebugResourceRemoveResponse(std::span<const std::byte> payload)
    {
        ByteReader                  reader {payload};
        DebugResourceRemoveResponse message;
        message.id      = reader.readU32();
        message.removed = reader.readU8() != 0;
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeDebugContinueRequest(const DebugContinueRequest&)
    {
        return {};
    }

    std::optional<DebugContinueRequest> decodeDebugContinueRequest(std::span<const std::byte> payload)
    {
        if (!payload.empty())
        {
            return std::nullopt;
        }
        return DebugContinueRequest {};
    }

    std::vector<std::byte> encodeDebugContinueResponse(const DebugContinueResponse& message)
    {
        return encodeDebugStatus(message);
    }

    std::optional<DebugContinueResponse> decodeDebugContinueResponse(std::span<const std::byte> payload)
    {
        return decodeDebugStatus(payload);
    }

    std::vector<std::byte> encodeDebugWaitRequest(const DebugWaitRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.timeoutMs);
        writer.writeU32(message.maxHits);
        return writer.take();
    }

    std::optional<DebugWaitRequest> decodeDebugWaitRequest(std::span<const std::byte> payload)
    {
        ByteReader       reader {payload};
        DebugWaitRequest message;
        message.timeoutMs = reader.readU32();
        message.maxHits   = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeDebugWaitHit(const DebugWaitHit& message)
    {
        ByteWriter writer;
        writer.writeU32(message.id);
        writer.writeU8(static_cast<std::uint8_t>(message.kind));
        writer.writeU8(static_cast<std::uint8_t>(message.breakpointKind));
        writer.writeU64(message.address);
        writeDebugRecord(writer, message.record);
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

    std::optional<DebugWaitHit> decodeDebugWaitHit(std::span<const std::byte> payload)
    {
        ByteReader   reader {payload};
        DebugWaitHit message;
        message.id              = reader.readU32();
        message.kind            = static_cast<DebugResourceKind>(reader.readU8());
        message.breakpointKind  = static_cast<DebugBreakpointKind>(reader.readU8());
        message.address         = reader.readU64();
        message.record          = readDebugRecord(reader);
        message.module          = reader.readString();
        message.moduleOffset    = reader.readU64();
        message.exportFound     = reader.readU8() != 0;
        message.exportAddress   = reader.readU64();
        message.exportOffset    = reader.readU64();
        message.exportName      = reader.readString();
        message.pdbFound        = reader.readU8() != 0;
        message.pdbSymbol       = reader.readString();
        message.pdbDisplacement = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        if (message.kind != DebugResourceKind::Breakpoint && message.kind != DebugResourceKind::Watchpoint)
        {
            return std::nullopt;
        }
        if (message.breakpointKind != DebugBreakpointKind::Software
            && message.breakpointKind != DebugBreakpointKind::Hardware)
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeDebugWaitEnd(const DebugWaitEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(static_cast<std::uint8_t>(message.status));
        writer.writeU32(message.hitCount);
        writer.writeU32(message.skippedCount);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<DebugWaitEnd> decodeDebugWaitEnd(std::span<const std::byte> payload)
    {
        ByteReader   reader {payload};
        DebugWaitEnd message;
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
} // namespace mpfshost
