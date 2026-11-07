#ifndef MPFSHOST_CORE_SERVER_MAP_BATCHING_HPP
#define MPFSHOST_CORE_SERVER_MAP_BATCHING_HPP

#pragma once

#include "core/ipc/message.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mpfshost
{
    [[nodiscard]] inline std::uint64_t vfsEntryEncodedSize(const VfsEntry& entry)
    {
        // name length prefix + name + size + three FILETIMEs + isDirectory
        constexpr std::uint64_t kFixed = sizeof(std::uint32_t) + sizeof(std::uint64_t) * 4 + sizeof(std::uint8_t);
        return kFixed + entry.name.size();
    }

    [[nodiscard]] inline std::uint64_t handleEncodedSize(const HandleDescriptor& handle)
    {
        // object, handle, access, type, three counters, two object addresses, pool tag plus two string prefixes
        constexpr std::uint64_t kFixed =
            sizeof(std::uint64_t) * 5 + sizeof(std::uint32_t) * 3 + sizeof(std::uint8_t) + sizeof(std::uint32_t) * 2;
        return kFixed + handle.text.size() + handle.type.size();
    }

    [[nodiscard]] inline std::uint64_t exportEncodedSize(const ExportDescriptor& entry)
    {
        // function address, ordinal, two array offsets plus two string prefixes
        constexpr std::uint64_t kFixed = sizeof(std::uint64_t) + sizeof(std::uint32_t) * 3 + sizeof(std::uint32_t) * 2;
        return kFixed + entry.function.size() + entry.forwardedFunction.size();
    }

    [[nodiscard]] inline std::uint64_t importEncodedSize(const ImportDescriptor& entry)
    {
        // function address, 32-bit flag, hint, two thunk RVAs plus two string prefixes
        constexpr std::uint64_t kFixed = sizeof(std::uint64_t) + sizeof(std::uint8_t) + sizeof(std::uint16_t)
                                       + sizeof(std::uint32_t) * 2 + sizeof(std::uint32_t) * 2;
        return kFixed + entry.function.size() + entry.module.size();
    }

    [[nodiscard]] inline std::uint64_t unloadedModuleEncodedSize(const UnloadedModuleDescriptor& module)
    {
        // base, image size, WoW64 flag, checksum, timestamp, unload time plus one string prefix
        constexpr std::uint64_t kFixed =
            sizeof(std::uint64_t) * 2 + sizeof(std::uint32_t) * 3 + sizeof(std::uint8_t) + sizeof(std::uint32_t);
        return kFixed + module.text.size();
    }

    [[nodiscard]] inline std::uint64_t serviceEncodedSize(const ServiceDescriptor& service)
    {
        // object address, ordinal, start type, the seven SERVICE_STATUS fields, the pid plus six string prefixes
        constexpr std::uint64_t kFixed = sizeof(std::uint64_t) + sizeof(std::uint32_t) * 16;
        return kFixed + service.name.size() + service.displayName.size() + service.imagePath.size()
             + service.path.size() + service.userType.size() + service.account.size();
    }

    [[nodiscard]] inline std::uint64_t userEncodedSize(const UserDescriptor& user)
    {
        // registry hive plus two string prefixes
        constexpr std::uint64_t kFixed = sizeof(std::uint64_t) + sizeof(std::uint32_t) * 2;
        return kFixed + user.name.size() + user.sid.size();
    }

    [[nodiscard]] inline std::uint64_t networkEntryEncodedSize(const NetworkEntryDescriptor& entry)
    {
        // pid, state, pool tag and the three string prefixes, the address family, two endpoint validity bytes, the
        // object address and the creation time
        constexpr std::uint64_t kFixed =
            sizeof(std::uint32_t) * 6 + sizeof(std::uint16_t) + sizeof(std::uint8_t) * 2 + sizeof(std::uint64_t) * 2;
        return kFixed + entry.process.size() + entry.local.address.size() + entry.remote.address.size();
    }

    [[nodiscard]] inline std::uint64_t heapEncodedSize(const HeapDescriptor&)
    {
        // address, index, heap number, type and the 32-bit flag
        return sizeof(std::uint64_t) + sizeof(std::uint32_t) * 2 + sizeof(std::uint8_t) * 2;
    }

    [[nodiscard]] inline std::uint64_t heapAllocEncodedSize(const HeapAllocDescriptor&)
    {
        // address, size and type
        return sizeof(std::uint64_t) + sizeof(std::uint32_t) + sizeof(std::uint8_t);
    }

    [[nodiscard]] inline std::uint64_t kernelDriverEncodedSize(const KernelDriverDescriptor& driver)
    {
        // four addresses, the dispatch array count prefix and its entries plus three string prefixes
        constexpr std::uint64_t kFixed = sizeof(std::uint64_t) * 4 + sizeof(std::uint32_t) * 4;
        return kFixed + driver.majorFunctions.size() * sizeof(std::uint64_t) + driver.name.size() + driver.path.size()
             + driver.serviceKeyName.size();
    }

    [[nodiscard]] inline std::uint64_t kernelDeviceEncodedSize(const KernelDeviceDescriptor& device)
    {
        // address, depth, device type, three more addresses plus the two string length prefixes
        constexpr std::uint64_t kFixed = sizeof(std::uint64_t) * 4 + sizeof(std::uint32_t) * 4;
        return kFixed + device.deviceTypeName.size() + device.volumeInfo.size();
    }

    [[nodiscard]] inline std::uint64_t kernelObjectEncodedSize(const KernelObjectDescriptor& object)
    {
        // address, parent, child count, the truncated byte, the child array count prefix and its entries plus two
        // string prefixes
        constexpr std::uint64_t kFixed = sizeof(std::uint64_t) * 3 + sizeof(std::uint8_t) + sizeof(std::uint32_t) * 3;
        return kFixed + object.children.size() * sizeof(std::uint64_t) + object.name.size() + object.type.size();
    }

    [[nodiscard]] inline std::uint64_t pteEncodedSize(const PteDescriptor& region)
    {
        // base, page count, page flags, software pages, the WoW64 byte plus one string prefix
        constexpr std::uint64_t kFixed = sizeof(std::uint64_t) * 3 + sizeof(std::uint32_t) * 2 + sizeof(std::uint8_t);
        return kFixed + region.text.size();
    }

    [[nodiscard]] inline std::uint64_t vadExEncodedSize(const VadExDescriptor&)
    {
        // page type, PML, page flags, VA, PA, PTE, prototype type, prototype PA, prototype PTE and the VAD base
        return sizeof(std::uint32_t) * 2 + sizeof(std::uint8_t) * 2 + sizeof(std::uint64_t) * 6;
    }

    [[nodiscard]] inline std::uint64_t poolEncodedSize(const PoolDescriptor&)
    {
        // address, size, tag, the alloc byte, the pool type and the subsegment
        return sizeof(std::uint64_t) + sizeof(std::uint32_t) * 2 + sizeof(std::uint8_t) * 3;
    }

    [[nodiscard]] inline std::uint64_t poolTagEncodedSize(const PoolTagDescriptor&)
    {
        // tag and the number of allocations carrying it
        return sizeof(std::uint32_t) * 2;
    }

    [[nodiscard]] inline std::uint64_t pfnEncodedSize(const PfnDescriptor&)
    {
        // pfn, extended type, page location, pid, VA, PTE, original PTE, PTE frame, reference count,
        // cache attribute, priority, flags and page colour
        return sizeof(std::uint32_t) * 3 + sizeof(std::uint8_t) * 6 + sizeof(std::uint64_t) * 3 + sizeof(std::uint16_t);
    }

    // Returns how many entries starting at offset fit inside budget encoded bytes. At least one entry is always
    // returned while entries remain, so an oversized entry can never stall the stream or truncate the count.
    template<typename Entry, typename SizeFunction>
    [[nodiscard]] std::size_t
    batchCount(const std::vector<Entry>& entries, std::size_t offset, std::uint32_t budget, SizeFunction encodedSize)
    {
        if (offset >= entries.size())
        {
            return 0;
        }

        std::size_t   count = 0;
        std::uint64_t total = sizeof(std::uint32_t);
        while (offset + count < entries.size())
        {
            const std::uint64_t next = encodedSize(entries[offset + count]);
            if (count > 0 && total + next > budget)
            {
                break;
            }
            total += next;
            ++count;
        }
        return count;
    }
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_MAP_BATCHING_HPP
