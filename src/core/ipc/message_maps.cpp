#include "core/ipc/message.hpp"
#include "core/ipc/message_codec.hpp"

namespace mpfshost
{
    using namespace detail;

    std::vector<std::byte> encodeModuleListRequest(const ModuleListRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeU16(message.flags);
        writer.writeString(message.nameFilter);
        return writer.take();
    }

    std::optional<ModuleListRequest> decodeModuleListRequest(std::span<const std::byte> payload)
    {
        ByteReader        reader {payload};
        ModuleListRequest message;
        message.pid        = reader.readU32();
        message.flags      = reader.readU16();
        message.nameFilter = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        if ((message.flags & ~(kModuleFlagCounts | kModuleFlagDebugInfo | kModuleFlagVersionInfo)) != 0)
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeModuleListData(const ModuleListData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.modules.size()));
        for (const auto& module : message.modules)
        {
            writeModule(writer, module);
        }
        return writer.take();
    }

    std::optional<ModuleListData> decodeModuleListData(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        ModuleListData message;
        message.modules.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            ModuleDescriptor module;
            if (!readModule(reader, module))
            {
                return std::nullopt;
            }
            message.modules.push_back(std::move(module));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeModuleListEnd(const ModuleListEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.moduleCount);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<ModuleListEnd> decodeModuleListEnd(std::span<const std::byte> payload)
    {
        ByteReader    reader {payload};
        ModuleListEnd message;
        message.status      = reader.readU8();
        message.moduleCount = reader.readU64();
        message.error       = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeMemoryMapRequest(const MemoryMapRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeU8(message.identifyModules ? 1 : 0);
        return writer.take();
    }

    std::optional<MemoryMapRequest> decodeMemoryMapRequest(std::span<const std::byte> payload)
    {
        ByteReader       reader {payload};
        MemoryMapRequest message;
        message.pid             = reader.readU32();
        message.identifyModules = reader.readU8() != 0;
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeMemoryMapData(const MemoryMapData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.regions.size()));
        for (const auto& region : message.regions)
        {
            writeRegion(writer, region);
        }
        return writer.take();
    }

    std::optional<MemoryMapData> decodeMemoryMapData(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        MemoryMapData message;
        message.regions.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            MemoryMapRegion region;
            if (!readRegion(reader, region))
            {
                return std::nullopt;
            }
            message.regions.push_back(std::move(region));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeMemoryMapEnd(const MemoryMapEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.regionCount);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<MemoryMapEnd> decodeMemoryMapEnd(std::span<const std::byte> payload)
    {
        ByteReader   reader {payload};
        MemoryMapEnd message;
        message.status      = reader.readU8();
        message.regionCount = reader.readU64();
        message.error       = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeVadExListRequest(const VadExListRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeU32(message.pageOffset);
        writer.writeU32(message.pageCount);
        return writer.take();
    }

    std::optional<VadExListRequest> decodeVadExListRequest(std::span<const std::byte> payload)
    {
        ByteReader       reader {payload};
        VadExListRequest message;
        message.pid        = reader.readU32();
        message.pageOffset = reader.readU32();
        message.pageCount  = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeVadExListData(const VadExListData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.entries.size()));
        for (const auto& entry : message.entries)
        {
            writeVadEx(writer, entry);
        }
        return writer.take();
    }

    std::optional<VadExListData> decodeVadExListData(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        VadExListData message;
        message.entries.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            VadExDescriptor entry;
            if (!readVadEx(reader, entry))
            {
                return std::nullopt;
            }
            message.entries.push_back(entry);
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeVadExListEnd(const VadExListEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.entryCount);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<VadExListEnd> decodeVadExListEnd(std::span<const std::byte> payload)
    {
        ByteReader   reader {payload};
        VadExListEnd message;
        message.status     = reader.readU8();
        message.entryCount = reader.readU64();
        message.error      = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeThreadListRequest(const ThreadListRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeU32(message.tid);
        writer.writeU16(message.flags);
        return writer.take();
    }

    std::optional<ThreadListRequest> decodeThreadListRequest(std::span<const std::byte> payload)
    {
        ByteReader        reader {payload};
        ThreadListRequest message;
        message.pid   = reader.readU32();
        message.tid   = reader.readU32();
        message.flags = reader.readU16();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        if ((message.flags & ~kThreadFlagCallstack) != 0)
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeThreadListData(const ThreadListData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.threads.size()));
        for (const auto& thread : message.threads)
        {
            writeThread(writer, thread);
        }
        return writer.take();
    }

    std::optional<ThreadListData> decodeThreadListData(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        ThreadListData message;
        message.threads.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            ThreadDescriptor thread;
            if (!readThread(reader, thread, payload.size()))
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

    std::vector<std::byte> encodeThreadListEnd(const ThreadListEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.threadCount);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<ThreadListEnd> decodeThreadListEnd(std::span<const std::byte> payload)
    {
        ByteReader    reader {payload};
        ThreadListEnd message;
        message.status      = reader.readU8();
        message.threadCount = reader.readU64();
        message.error       = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeModuleLayoutRequest(const ModuleLayoutRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeString(message.module);
        return writer.take();
    }

    std::optional<ModuleLayoutRequest> decodeModuleLayoutRequest(std::span<const std::byte> payload)
    {
        ByteReader          reader {payload};
        ModuleLayoutRequest message;
        message.pid = reader.readU32();
        if (!readCappedString(reader, message.module) || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeModuleLayoutResponse(const ModuleLayoutResponse& message)
    {
        ByteWriter writer;
        writer.writeU64(message.moduleBase);
        writer.writeU32(static_cast<std::uint32_t>(message.sections.size()));
        for (const auto& section : message.sections)
        {
            writer.writeString(section.name);
            writer.writeU64(section.virtualAddress);
            writer.writeU32(section.virtualSize);
            writer.writeU32(section.rawPointer);
            writer.writeU32(section.rawSize);
            writer.writeU32(section.characteristics);
        }
        writer.writeU32(static_cast<std::uint32_t>(message.directories.size()));
        for (const auto& directory : message.directories)
        {
            writer.writeU32(directory.index);
            writer.writeU32(directory.virtualAddress);
            writer.writeU32(directory.size);
        }
        return writer.take();
    }

    std::optional<ModuleLayoutResponse> decodeModuleLayoutResponse(std::span<const std::byte> payload)
    {
        ByteReader           reader {payload};
        ModuleLayoutResponse message;
        message.moduleBase      = reader.readU64();
        const auto sectionCount = reader.readU32();
        if (!reader.ok() || sectionCount > kMaxModuleSections)
        {
            return std::nullopt;
        }

        message.sections.reserve(sectionCount);
        for (std::uint32_t i = 0; i < sectionCount; ++i)
        {
            ModuleSection section;
            if (!readCappedString(reader, section.name))
            {
                return std::nullopt;
            }
            section.virtualAddress  = reader.readU64();
            section.virtualSize     = reader.readU32();
            section.rawPointer      = reader.readU32();
            section.rawSize         = reader.readU32();
            section.characteristics = reader.readU32();
            if (!reader.ok())
            {
                return std::nullopt;
            }
            message.sections.push_back(std::move(section));
        }

        const auto directoryCount = reader.readU32();
        if (!reader.ok() || directoryCount > kModuleDataDirectoryCount)
        {
            return std::nullopt;
        }

        message.directories.reserve(directoryCount);
        for (std::uint32_t i = 0; i < directoryCount; ++i)
        {
            ModuleDataDirectory directory;
            directory.index          = reader.readU32();
            directory.virtualAddress = reader.readU32();
            directory.size           = reader.readU32();
            if (!reader.ok())
            {
                return std::nullopt;
            }
            message.directories.push_back(directory);
        }

        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeModuleByAddressRequest(const ModuleByAddressRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeU64(message.address);
        return writer.take();
    }

    std::optional<ModuleByAddressRequest> decodeModuleByAddressRequest(std::span<const std::byte> payload)
    {
        ByteReader             reader {payload};
        ModuleByAddressRequest message;
        message.pid     = reader.readU32();
        message.address = reader.readU64();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeModuleByAddressResponse(const ModuleByAddressResponse& message)
    {
        ByteWriter writer;
        writer.writeU64(message.base);
        writer.writeU64(message.entry);
        writer.writeU64(message.imageSize);
        writer.writeU64(message.offset);
        writer.writeU16(message.kind);
        writer.writeU8(message.wow64 ? 1 : 0);
        writer.writeString(message.name);
        writer.writeString(message.fullName);
        return writer.take();
    }

    std::optional<ModuleByAddressResponse> decodeModuleByAddressResponse(std::span<const std::byte> payload)
    {
        ByteReader              reader {payload};
        ModuleByAddressResponse message;
        message.base      = reader.readU64();
        message.entry     = reader.readU64();
        message.imageSize = reader.readU64();
        message.offset    = reader.readU64();
        message.kind      = reader.readU16();
        message.wow64     = reader.readU8() != 0;
        if (!readCappedString(reader, message.name) || !readCappedString(reader, message.fullName) || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeExportListRequest(const ExportListRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeString(message.module);
        return writer.take();
    }

    std::optional<ExportListRequest> decodeExportListRequest(std::span<const std::byte> payload)
    {
        ByteReader        reader {payload};
        ExportListRequest message;
        message.pid    = reader.readU32();
        message.module = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeExportListData(const ExportListData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.exports.size()));
        for (const auto& entry : message.exports)
        {
            writeExport(writer, entry);
        }
        return writer.take();
    }

    std::optional<ExportListData> decodeExportListData(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        ExportListData message;
        message.exports.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            ExportDescriptor entry;
            if (!readExport(reader, entry))
            {
                return std::nullopt;
            }
            message.exports.push_back(std::move(entry));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeExportListEnd(const ExportListEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.exportCount);
        writer.writeU8(message.truncated);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<ExportListEnd> decodeExportListEnd(std::span<const std::byte> payload)
    {
        ByteReader    reader {payload};
        ExportListEnd message;
        message.status      = reader.readU8();
        message.exportCount = reader.readU64();
        message.truncated   = reader.readU8();
        message.error       = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeImportListRequest(const ImportListRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeString(message.module);
        return writer.take();
    }

    std::optional<ImportListRequest> decodeImportListRequest(std::span<const std::byte> payload)
    {
        ByteReader        reader {payload};
        ImportListRequest message;
        message.pid    = reader.readU32();
        message.module = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeImportListData(const ImportListData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.imports.size()));
        for (const auto& entry : message.imports)
        {
            writeImport(writer, entry);
        }
        return writer.take();
    }

    std::optional<ImportListData> decodeImportListData(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        ImportListData message;
        message.imports.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            ImportDescriptor entry;
            if (!readImport(reader, entry))
            {
                return std::nullopt;
            }
            message.imports.push_back(std::move(entry));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeImportListEnd(const ImportListEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.importCount);
        writer.writeU8(message.truncated);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<ImportListEnd> decodeImportListEnd(std::span<const std::byte> payload)
    {
        ByteReader    reader {payload};
        ImportListEnd message;
        message.status      = reader.readU8();
        message.importCount = reader.readU64();
        message.truncated   = reader.readU8();
        message.error       = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeUnloadedModuleListRequest(const UnloadedModuleListRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeString(message.filter);
        return writer.take();
    }

    std::optional<UnloadedModuleListRequest> decodeUnloadedModuleListRequest(std::span<const std::byte> payload)
    {
        ByteReader                reader {payload};
        UnloadedModuleListRequest message;
        message.pid    = reader.readU32();
        message.filter = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeUnloadedModuleListData(const UnloadedModuleListData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.modules.size()));
        for (const auto& module : message.modules)
        {
            writeUnloadedModule(writer, module);
        }
        return writer.take();
    }

    std::optional<UnloadedModuleListData> decodeUnloadedModuleListData(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        UnloadedModuleListData message;
        message.modules.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            UnloadedModuleDescriptor module;
            if (!readUnloadedModule(reader, module))
            {
                return std::nullopt;
            }
            message.modules.push_back(std::move(module));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeUnloadedModuleListEnd(const UnloadedModuleListEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.moduleCount);
        writer.writeU8(message.truncated);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<UnloadedModuleListEnd> decodeUnloadedModuleListEnd(std::span<const std::byte> payload)
    {
        ByteReader            reader {payload};
        UnloadedModuleListEnd message;
        message.status      = reader.readU8();
        message.moduleCount = reader.readU64();
        message.truncated   = reader.readU8();
        message.error       = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

} // namespace mpfshost
