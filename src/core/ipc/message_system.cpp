#include "core/ipc/message.hpp"
#include "core/ipc/message_codec.hpp"

namespace mpfshost
{
    using namespace detail;

    std::vector<std::byte> encodeVmListResponse(const VmListResponse& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.vms.size()));
        for (const auto& vm : message.vms)
        {
            writeVm(writer, vm);
        }
        return writer.take();
    }

    std::optional<VmListResponse> decodeVmListResponse(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        VmListResponse message;
        message.vms.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            VmDescriptor vm;
            if (!readVm(reader, vm))
            {
                return std::nullopt;
            }
            message.vms.push_back(std::move(vm));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePhysicalReadRequest(const PhysicalReadRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.vmIndex);
        writer.writeU64(message.gpa);
        writer.writeU32(message.size);
        return writer.take();
    }

    std::optional<PhysicalReadRequest> decodePhysicalReadRequest(std::span<const std::byte> payload)
    {
        ByteReader          reader {payload};
        PhysicalReadRequest message;
        message.vmIndex = reader.readU32();
        message.gpa     = reader.readU64();
        message.size    = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePhysicalReadEnd(const PhysicalReadEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.bytesDelivered);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<PhysicalReadEnd> decodePhysicalReadEnd(std::span<const std::byte> payload)
    {
        ByteReader      reader {payload};
        PhysicalReadEnd message;
        message.status         = reader.readU8();
        message.bytesDelivered = reader.readU64();
        message.error          = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePhysicalWriteRequest(const PhysicalWriteRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.vmIndex);
        writer.writeU64(message.gpa);
        writer.writeBytes(message.data);
        return writer.take();
    }

    std::optional<PhysicalWriteRequest> decodePhysicalWriteRequest(std::span<const std::byte> payload)
    {
        ByteReader           reader {payload};
        PhysicalWriteRequest message;
        message.vmIndex = reader.readU32();
        message.gpa     = reader.readU64();
        message.data    = reader.readBytes();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeGpaTranslateRequest(const GpaTranslateRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.vmIndex);
        writer.writeU64(message.gpa);
        return writer.take();
    }

    std::optional<GpaTranslateRequest> decodeGpaTranslateRequest(std::span<const std::byte> payload)
    {
        ByteReader          reader {payload};
        GpaTranslateRequest message;
        message.vmIndex = reader.readU32();
        message.gpa     = reader.readU64();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeGpaTranslateResponse(const GpaTranslateResponse& message)
    {
        ByteWriter writer;
        writer.writeU64(message.pa);
        writer.writeU64(message.va);
        writer.writeU8(message.paPresent ? 1 : 0);
        writer.writeU8(message.vaPresent ? 1 : 0);
        return writer.take();
    }

    std::optional<GpaTranslateResponse> decodeGpaTranslateResponse(std::span<const std::byte> payload)
    {
        ByteReader           reader {payload};
        GpaTranslateResponse message;
        message.pa        = reader.readU64();
        message.va        = reader.readU64();
        message.paPresent = reader.readU8() != 0;
        message.vaPresent = reader.readU8() != 0;
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeVirt2PhysRequest(const Virt2PhysRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeU64(message.va);
        return writer.take();
    }

    std::optional<Virt2PhysRequest> decodeVirt2PhysRequest(std::span<const std::byte> payload)
    {
        ByteReader       reader {payload};
        Virt2PhysRequest message;
        message.pid = reader.readU32();
        message.va  = reader.readU64();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeVirt2PhysResponse(const Virt2PhysResponse& message)
    {
        ByteWriter writer;
        writer.writeU64(message.pa);
        return writer.take();
    }

    std::optional<Virt2PhysResponse> decodeVirt2PhysResponse(std::span<const std::byte> payload)
    {
        ByteReader        reader {payload};
        Virt2PhysResponse message;
        message.pa = reader.readU64();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodePhysicalScatterRequest(const PhysicalScatterRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.vmIndex);
        writer.writeU32(static_cast<std::uint32_t>(message.entries.size()));
        for (const auto& entry : message.entries)
        {
            writeScatterEntry(writer, entry);
        }
        return writer.take();
    }

    std::optional<PhysicalScatterRequest> decodePhysicalScatterRequest(std::span<const std::byte> payload)
    {
        ByteReader             reader {payload};
        PhysicalScatterRequest message;
        message.vmIndex  = reader.readU32();
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        message.entries.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            ScatterEntry entry;
            if (!readScatterEntry(reader, entry))
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

    std::vector<std::byte> encodeServiceListRequest(const ServiceListRequest& message)
    {
        ByteWriter writer;
        writer.writeString(message.filter);
        return writer.take();
    }

    std::optional<ServiceListRequest> decodeServiceListRequest(std::span<const std::byte> payload)
    {
        ByteReader         reader {payload};
        ServiceListRequest message;
        message.filter = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeServiceListData(const ServiceListData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.services.size()));
        for (const auto& service : message.services)
        {
            writeService(writer, service);
        }
        return writer.take();
    }

    std::optional<ServiceListData> decodeServiceListData(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        ServiceListData message;
        message.services.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            ServiceDescriptor service;
            if (!readService(reader, service))
            {
                return std::nullopt;
            }
            message.services.push_back(std::move(service));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeServiceListEnd(const ServiceListEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.serviceCount);
        writer.writeU8(message.truncated);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<ServiceListEnd> decodeServiceListEnd(std::span<const std::byte> payload)
    {
        ByteReader     reader {payload};
        ServiceListEnd message;
        message.status       = reader.readU8();
        message.serviceCount = reader.readU64();
        message.truncated    = reader.readU8();
        message.error        = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeUserListRequest(const UserListRequest& message)
    {
        ByteWriter writer;
        writer.writeString(message.filter);
        return writer.take();
    }

    std::optional<UserListRequest> decodeUserListRequest(std::span<const std::byte> payload)
    {
        ByteReader      reader {payload};
        UserListRequest message;
        message.filter = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeUserListData(const UserListData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.users.size()));
        for (const auto& user : message.users)
        {
            writeUser(writer, user);
        }
        return writer.take();
    }

    std::optional<UserListData> decodeUserListData(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        UserListData message;
        message.users.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            UserDescriptor user;
            if (!readUser(reader, user))
            {
                return std::nullopt;
            }
            message.users.push_back(std::move(user));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeUserListEnd(const UserListEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.userCount);
        writer.writeU8(message.truncated);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<UserListEnd> decodeUserListEnd(std::span<const std::byte> payload)
    {
        ByteReader  reader {payload};
        UserListEnd message;
        message.status    = reader.readU8();
        message.userCount = reader.readU64();
        message.truncated = reader.readU8();
        message.error     = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeNetworkListRequest(const NetworkListRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeString(message.filter);
        return writer.take();
    }

    std::optional<NetworkListRequest> decodeNetworkListRequest(std::span<const std::byte> payload)
    {
        ByteReader         reader {payload};
        NetworkListRequest message;
        message.pid    = reader.readU32();
        message.filter = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeNetworkListData(const NetworkListData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.entries.size()));
        for (const auto& entry : message.entries)
        {
            writeNetworkEntry(writer, entry);
        }
        return writer.take();
    }

    std::optional<NetworkListData> decodeNetworkListData(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        NetworkListData message;
        message.entries.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            NetworkEntryDescriptor entry;
            if (!readNetworkEntry(reader, entry))
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

    std::vector<std::byte> encodeNetworkListEnd(const NetworkListEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.entryCount);
        writer.writeU8(message.truncated);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<NetworkListEnd> decodeNetworkListEnd(std::span<const std::byte> payload)
    {
        ByteReader     reader {payload};
        NetworkListEnd message;
        message.status     = reader.readU8();
        message.entryCount = reader.readU64();
        message.truncated  = reader.readU8();
        message.error      = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeHeapListRequest(const HeapListRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        return writer.take();
    }

    std::optional<HeapListRequest> decodeHeapListRequest(std::span<const std::byte> payload)
    {
        ByteReader      reader {payload};
        HeapListRequest message;
        message.pid = reader.readU32();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeHeapListData(const HeapListData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.heaps.size()));
        for (const auto& heap : message.heaps)
        {
            writeHeap(writer, heap);
        }
        return writer.take();
    }

    std::optional<HeapListData> decodeHeapListData(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        HeapListData message;
        message.heaps.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            HeapDescriptor heap;
            if (!readHeap(reader, heap))
            {
                return std::nullopt;
            }
            message.heaps.push_back(std::move(heap));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeHeapListEnd(const HeapListEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.heapCount);
        writer.writeU8(message.truncated);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<HeapListEnd> decodeHeapListEnd(std::span<const std::byte> payload)
    {
        ByteReader  reader {payload};
        HeapListEnd message;
        message.status    = reader.readU8();
        message.heapCount = reader.readU64();
        message.truncated = reader.readU8();
        message.error     = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeHeapAllocListRequest(const HeapAllocListRequest& message)
    {
        ByteWriter writer;
        writer.writeU32(message.pid);
        writer.writeU64(message.heap);
        return writer.take();
    }

    std::optional<HeapAllocListRequest> decodeHeapAllocListRequest(std::span<const std::byte> payload)
    {
        ByteReader           reader {payload};
        HeapAllocListRequest message;
        message.pid  = reader.readU32();
        message.heap = reader.readU64();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeHeapAllocListData(const HeapAllocListData& message)
    {
        ByteWriter writer;
        writer.writeU32(static_cast<std::uint32_t>(message.allocations.size()));
        for (const auto& allocation : message.allocations)
        {
            writeHeapAlloc(writer, allocation);
        }
        return writer.take();
    }

    std::optional<HeapAllocListData> decodeHeapAllocListData(std::span<const std::byte> payload)
    {
        ByteReader reader {payload};
        const auto count = reader.readU32();
        if (!reader.ok() || count > payload.size())
        {
            return std::nullopt;
        }

        HeapAllocListData message;
        message.allocations.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            HeapAllocDescriptor allocation;
            if (!readHeapAlloc(reader, allocation))
            {
                return std::nullopt;
            }
            message.allocations.push_back(std::move(allocation));
        }
        if (!reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    std::vector<std::byte> encodeHeapAllocListEnd(const HeapAllocListEnd& message)
    {
        ByteWriter writer;
        writer.writeU8(message.status);
        writer.writeU64(message.allocationCount);
        writer.writeU8(message.truncated);
        writer.writeString(message.error);
        return writer.take();
    }

    std::optional<HeapAllocListEnd> decodeHeapAllocListEnd(std::span<const std::byte> payload)
    {
        ByteReader       reader {payload};
        HeapAllocListEnd message;
        message.status          = reader.readU8();
        message.allocationCount = reader.readU64();
        message.truncated       = reader.readU8();
        message.error           = reader.readString();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

    namespace
    {
        struct ConfigOptionInfo
        {
            ConfigOption     option;
            std::string_view name;
            bool             usesPid;
            bool             requiresValue;
        };

        constexpr ConfigOptionInfo kConfigOptions[] = {
            {                ConfigOption::TickPeriod,                   "tick-period", false,  true},
            {            ConfigOption::ReadCacheTicks,               "readcache-ticks", false,  true},
            {             ConfigOption::TlbCacheTicks,                "tlbcache-ticks", false,  true},
            {     ConfigOption::ProcCacheTicksPartial,       "proccache-ticks-partial", false,  true},
            {       ConfigOption::ProcCacheTicksTotal,         "proccache-ticks-total", false,  true},
            {                ConfigOption::RefreshAll,                   "refresh-all", false, false},
            {            ConfigOption::RefreshFreqMem,              "refresh-freq-mem", false, false},
            {     ConfigOption::RefreshFreqMemPartial,      "refresh-freq-mem-partial", false, false},
            {            ConfigOption::RefreshFreqTlb,              "refresh-freq-tlb", false, false},
            {     ConfigOption::RefreshFreqTlbPartial,      "refresh-freq-tlb-partial", false, false},
            {           ConfigOption::RefreshFreqFast,             "refresh-freq-fast", false, false},
            {         ConfigOption::RefreshFreqMedium,           "refresh-freq-medium", false, false},
            {           ConfigOption::RefreshFreqSlow,             "refresh-freq-slow", false, false},
            {  ConfigOption::RefreshSpecificHeapAlloc,   "refresh-specific-heap-alloc", false, false},
            {    ConfigOption::RefreshSpecificKobject,      "refresh-specific-kobject", false, false},
            {        ConfigOption::RefreshSpecificNet,          "refresh-specific-net", false, false},
            {        ConfigOption::RefreshSpecificPfn,          "refresh-specific-pfn", false, false},
            { ConfigOption::RefreshSpecificPhysmemmap,   "refresh-specific-physmemmap", false, false},
            {       ConfigOption::RefreshSpecificPool,         "refresh-specific-pool", false, false},
            {   ConfigOption::RefreshSpecificRegistry,     "refresh-specific-registry", false, false},
            {   ConfigOption::RefreshSpecificServices,     "refresh-specific-services", false, false},
            {   ConfigOption::RefreshSpecificThreadcs,     "refresh-specific-threadcs", false, false},
            {       ConfigOption::RefreshSpecificUser,         "refresh-specific-user", false, false},
            {         ConfigOption::RefreshSpecificVm,           "refresh-specific-vm", false, false},
            {    ConfigOption::RefreshSpecificProcess,      "refresh-specific-process",  true, false},
            {                ConfigOption::ProcessDtb,                   "process-dtb",  true,  true},
            {ConfigOption::ProcessDtbFastLowIntegrity, "process-dtb-fast-lowintegrity",  true,  true},
        };

        const ConfigOptionInfo* findConfigOption(ConfigOption option) noexcept
        {
            for (const auto& info : kConfigOptions)
            {
                if (info.option == option)
                {
                    return &info;
                }
            }
            return nullptr;
        }
    } // namespace

    std::optional<ConfigOption> parseConfigOption(std::string_view name) noexcept
    {
        for (const auto& info : kConfigOptions)
        {
            if (info.name == name)
            {
                return info.option;
            }
        }
        return std::nullopt;
    }

    std::string_view configOptionName(ConfigOption option) noexcept
    {
        const auto* info = findConfigOption(option);
        return info == nullptr ? std::string_view {} : info->name;
    }

    bool configOptionUsesPid(ConfigOption option) noexcept
    {
        const auto* info = findConfigOption(option);
        return info != nullptr && info->usesPid;
    }

    bool configOptionRequiresValue(ConfigOption option) noexcept
    {
        const auto* info = findConfigOption(option);
        return info != nullptr && info->requiresValue;
    }

    bool isValidConfigOption(std::uint8_t value) noexcept
    {
        return findConfigOption(static_cast<ConfigOption>(value)) != nullptr;
    }

    std::vector<std::byte> encodeConfigSetRequest(const ConfigSetRequest& message)
    {
        ByteWriter writer;
        writer.writeU8(static_cast<std::uint8_t>(message.option));
        writer.writeU32(message.pid);
        writer.writeU64(message.value);
        return writer.take();
    }

    std::optional<ConfigSetRequest> decodeConfigSetRequest(std::span<const std::byte> payload)
    {
        ByteReader       reader {payload};
        ConfigSetRequest message;
        const auto       option = reader.readU8();
        message.pid             = reader.readU32();
        message.value           = reader.readU64();
        if (!reader.ok() || !reader.atEnd() || !isValidConfigOption(option))
        {
            return std::nullopt;
        }
        message.option = static_cast<ConfigOption>(option);
        return message;
    }

    std::vector<std::byte> encodeConfigSetResponse(const ConfigSetResponse& message)
    {
        ByteWriter writer;
        writer.writeU64(message.value);
        return writer.take();
    }

    std::optional<ConfigSetResponse> decodeConfigSetResponse(std::span<const std::byte> payload)
    {
        ByteReader        reader {payload};
        ConfigSetResponse message;
        message.value = reader.readU64();
        if (!reader.ok() || !reader.atEnd())
        {
            return std::nullopt;
        }
        return message;
    }

} // namespace mpfshost
