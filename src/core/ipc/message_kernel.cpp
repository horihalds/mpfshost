#include "core/ipc/message.hpp"
#include "core/ipc/message_codec.hpp"

namespace mpfshost
{
    using namespace detail;

    std::vector<std::byte> encodeKDriverListRequest(const KDriverListRequest& message)
    {
        ByteWriter writer;
        writer.writeString(message.filter);
        return writer.take();
    }

    std::optional<KDriverListRequest> decodeKDriverListRequest(std::span<const std::byte> payload)
    {
        ByteReader         reader {payload};
        KDriverListRequest message;
        message.filter = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeKDriverListData(const KDriverListData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.drivers.size()));
        for (const auto& driver : message.drivers)
        {
            writeKernelDriver(writer, driver);
        }
        return writer.take();
    }

    std::optional<KDriverListData> decodeKDriverListData(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        KDriverListData message;
        message.drivers.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            KernelDriverDescriptor driver;
            if (!readKernelDriver(reader, driver))
            {
                return std::nullopt;
            }
            message.drivers.push_back(std::move(driver));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeKDriverListEnd(const KDriverListEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.driverCount);
        writer.writeU8(message.truncated);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<KDriverListEnd> decodeKDriverListEnd(std::span<const std::byte> payload)
    {
        ByteReader     reader {payload};
        KDriverListEnd message;
        message.status      = reader.readU8();
        message.driverCount = reader.readU64();
        message.truncated   = reader.readU8();
        message.error       = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeKDeviceListRequest(const KDeviceListRequest& message)
    {
        ByteWriter writer;
        writer.writeString(message.filter);
        return writer.take();
    }

    std::optional<KDeviceListRequest> decodeKDeviceListRequest(std::span<const std::byte> payload)
    {
        ByteReader         reader {payload};
        KDeviceListRequest message;
        message.filter = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeKDeviceListData(const KDeviceListData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.devices.size()));
        for (const auto& device : message.devices)
        {
            writeKernelDevice(writer, device);
        }
        return writer.take();
    }

    std::optional<KDeviceListData> decodeKDeviceListData(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        KDeviceListData message;
        message.devices.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            KernelDeviceDescriptor device;
            if (!readKernelDevice(reader, device))
            {
                return std::nullopt;
            }
            message.devices.push_back(std::move(device));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeKDeviceListEnd(const KDeviceListEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.deviceCount);
        writer.writeU8(message.truncated);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<KDeviceListEnd> decodeKDeviceListEnd(std::span<const std::byte> payload)
    {
        ByteReader     reader {payload};
        KDeviceListEnd message;
        message.status      = reader.readU8();
        message.deviceCount = reader.readU64();
        message.truncated   = reader.readU8();
        message.error       = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeKObjectListRequest(const KObjectListRequest& message)
    {
        ByteWriter writer;
        writer.writeString(message.filter);
        return writer.take();
    }

    std::optional<KObjectListRequest> decodeKObjectListRequest(std::span<const std::byte> payload)
    {
        ByteReader         reader {payload};
        KObjectListRequest message;
        message.filter = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeKObjectListData(const KObjectListData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.objects.size()));
        for (const auto& object : message.objects)
        {
            writeKernelObject(writer, object);
        }
        return writer.take();
    }

    std::optional<KObjectListData> decodeKObjectListData(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        KObjectListData message;
        message.objects.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            KernelObjectDescriptor object;
            if (!readKernelObject(reader, object))
            {
                return std::nullopt;
            }
            message.objects.push_back(std::move(object));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeKObjectListEnd(const KObjectListEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.objectCount);
        writer.writeU8(message.truncated);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<KObjectListEnd> decodeKObjectListEnd(std::span<const std::byte> payload)
    {
        ByteReader     reader {payload};
        KObjectListEnd message;
        message.status      = reader.readU8();
        message.objectCount = reader.readU64();
        message.truncated   = reader.readU8();
        message.error       = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePteListRequest(const PteListRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeU8(message.identifyModules);
        writer.writeU64(message.minAddress);
        writer.writeU64(message.maxAddress);
        writer.writeString(message.filter);
        return writer.take();
    }

    std::optional<PteListRequest> decodePteListRequest(std::span<const std::byte> payload)
    {
        ByteReader     reader {payload};
        PteListRequest message;
        message.pid             = reader.readU32();
        message.identifyModules = reader.readU8();
        message.minAddress      = reader.readU64();
        message.maxAddress      = reader.readU64();
        message.filter          = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePteListData(const PteListData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.entries.size()));
        for (const auto& entry : message.entries)
        {
            writePte(writer, entry);
        }
        return writer.take();
    }

    std::optional<PteListData> decodePteListData(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        PteListData message;
        message.entries.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            PteDescriptor entry;
            if (!readPte(reader, entry))
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

    std::vector<std::byte> encodePteListEnd(const PteListEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.entryCount);
        writer.writeU64(message.mapEntries);
        writer.writeU8(message.truncated);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<PteListEnd> decodePteListEnd(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        PteListEnd message;
        message.status     = reader.readU8();
        message.entryCount = reader.readU64();
        message.mapEntries = reader.readU64();
        message.truncated  = reader.readU8();
        message.error      = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePoolListRequest(const PoolListRequest& message)
    {
        ByteWriter writer;
        writer.writeU8(message.big);
        writer.writeString(message.tagFilter);
        writer.writeU32(message.minBytes);
        writer.writeU64(message.minAddress);
        writer.writeU64(message.maxAddress);
        return writer.take();
    }

    std::optional<PoolListRequest> decodePoolListRequest(std::span<const std::byte> payload)
    {
        ByteReader      reader {payload};
        PoolListRequest message;
        message.big        = reader.readU8();
        message.tagFilter  = reader.readString();
        message.minBytes   = reader.readU32();
        message.minAddress = reader.readU64();
        message.maxAddress = reader.readU64();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePoolListData(const PoolListData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.entries.size()));
        for (const auto& entry : message.entries)
        {
            writePool(writer, entry);
        }
        return writer.take();
    }

    std::optional<PoolListData> decodePoolListData(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        PoolListData message;
        message.entries.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            PoolDescriptor entry;
            if (!readPool(reader, entry))
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

    std::vector<std::byte> encodePoolListEnd(const PoolListEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.entryCount);
        writer.writeU64(message.mapEntries);
        writer.writeU64(message.mapBytes);
        writer.writeU8(message.truncated);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<PoolListEnd> decodePoolListEnd(std::span<const std::byte> payload)
    {
        ByteReader  reader {payload};
        PoolListEnd message;
        message.status     = reader.readU8();
        message.entryCount = reader.readU64();
        message.mapEntries = reader.readU64();
        message.mapBytes   = reader.readU64();
        message.truncated  = reader.readU8();
        message.error      = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePoolTagListRequest(const PoolTagListRequest& message)
    {
        ByteWriter writer;
        writer.writeU8(message.big);
        writer.writeString(message.tagFilter);
        return writer.take();
    }

    std::optional<PoolTagListRequest> decodePoolTagListRequest(std::span<const std::byte> payload)
    {
        ByteReader         reader {payload};
        PoolTagListRequest message;
        message.big       = reader.readU8();
        message.tagFilter = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePoolTagListData(const PoolTagListData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.tags.size()));
        for (const auto& entry : message.tags)
        {
            writePoolTag(writer, entry);
        }
        return writer.take();
    }

    std::optional<PoolTagListData> decodePoolTagListData(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        PoolTagListData message;
        message.tags.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            PoolTagDescriptor entry;
            if (!readPoolTag(reader, entry))
            {
                return std::nullopt;
            }
            message.tags.push_back(entry);
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePoolTagListEnd(const PoolTagListEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.entryCount);
        writer.writeU64(message.mapTags);
        writer.writeU64(message.mapEntries);
        writer.writeU8(message.truncated);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<PoolTagListEnd> decodePoolTagListEnd(std::span<const std::byte> payload)
    {
        ByteReader     reader {payload};
        PoolTagListEnd message;
        message.status     = reader.readU8();
        message.entryCount = reader.readU64();
        message.mapTags    = reader.readU64();
        message.mapEntries = reader.readU64();
        message.truncated  = reader.readU8();
        message.error      = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePfnListRequest(const PfnListRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pfnStart);
        writer.writeU32(message.pfnCount);
        return writer.take();
    }

    std::optional<PfnListRequest> decodePfnListRequest(std::span<const std::byte> payload)
    {
        ByteReader     reader {payload};
        PfnListRequest message;
        message.pfnStart = reader.readU32();
        message.pfnCount = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePfnListData(const PfnListData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.entries.size()));
        for (const auto& entry : message.entries)
        {
            writePfn(writer, entry);
        }
        return writer.take();
    }

    std::optional<PfnListData> decodePfnListData(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        PfnListData message;
        message.entries.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            PfnDescriptor entry;
            if (!readPfn(reader, entry))
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

    std::vector<std::byte> encodePfnListEnd(const PfnListEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.entryCount);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<PfnListEnd> decodePfnListEnd(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        PfnListEnd message;
        message.status     = reader.readU8();
        message.entryCount = reader.readU64();
        message.error      = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }
} // namespace mpfshost
