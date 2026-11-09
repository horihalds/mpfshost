#include "host/vmm_memory.hpp"

#include "core/server/request_deadline.hpp"

#include <algorithm>
#include <cstddef>

namespace mpfshost
{
    namespace
    {
        constexpr std::size_t   kMaxTransferChunk = 128 * 0x1000;
        constexpr std::uint64_t kPageOffsetMask   = 0xFFF;
    } // namespace

    bool readVmmMemory(VMM_HANDLE           handle,
                       std::uint32_t        pid,
                       std::uint64_t        address,
                       std::span<std::byte> destination,
                       std::string&         error)
    {
        if (handle == nullptr || pid == 0 || destination.empty())
        {
            error = "invalid memory read parameters";
            return false;
        }

        std::size_t offset = 0;
        while (offset < destination.size())
        {
            if (requestExpired())
            {
                error = "request deadline exceeded";
                return false;
            }
            const auto chunk = static_cast<DWORD>(std::min(kMaxTransferChunk, destination.size() - offset));
            DWORD      bytesRead {};
            const auto result = VMMDLL_MemReadEx(handle,
                                                 pid,
                                                 address + offset,
                                                 reinterpret_cast<PBYTE>(destination.data() + offset),
                                                 chunk,
                                                 &bytesRead,
                                                 VMMDLL_FLAG_NOCACHE | VMMDLL_FLAG_NOCACHEPUT);
            if (!result || bytesRead != chunk)
            {
                error = "VMMDLL_MemReadEx failed at " + std::to_string(address + offset);
                return false;
            }
            offset += chunk;
        }
        return true;
    }

    bool readVmmMemoryPartial(VMM_HANDLE              handle,
                              std::uint32_t           pid,
                              std::uint64_t           address,
                              std::uint32_t           size,
                              std::vector<std::byte>& out,
                              std::string&            error)
    {
        out.clear();
        if (handle == nullptr || pid == 0)
        {
            error = "invalid memory read parameters";
            return false;
        }
        if (size == 0)
        {
            return true;
        }

        out.reserve(size);
        while (out.size() < size)
        {
            if (requestExpired())
            {
                error = "request deadline exceeded";
                return false;
            }

            const auto wanted = std::min<std::uint32_t>(static_cast<std::uint32_t>(kPageSize), size - out.size());
            const auto page   = out.size();
            out.resize(page + wanted);
            DWORD      bytesRead {};
            const auto result = VMMDLL_MemReadEx(handle,
                                                 pid,
                                                 address + page,
                                                 reinterpret_cast<PBYTE>(out.data() + page),
                                                 wanted,
                                                 &bytesRead,
                                                 VMMDLL_FLAG_NOCACHE | VMMDLL_FLAG_NOCACHEPUT);
            if (!result || bytesRead != wanted)
            {
                out.resize(page);
                return true;
            }
        }
        return true;
    }

    bool writeVmmMemory(VMM_HANDLE                 handle,
                        std::uint32_t              pid,
                        std::uint64_t              address,
                        std::span<const std::byte> source,
                        std::string&               error)
    {
        if (handle == nullptr || pid == 0 || source.empty())
        {
            error = "invalid memory write parameters";
            return false;
        }

        std::size_t offset = 0;
        while (offset < source.size())
        {
            if (requestExpired())
            {
                error = "request deadline exceeded";
                return false;
            }
            const auto chunk = static_cast<DWORD>(std::min(kMaxTransferChunk, source.size() - offset));
            if (!VMMDLL_MemWrite(handle,
                                 pid,
                                 address + offset,
                                 reinterpret_cast<PBYTE>(const_cast<std::byte*>(source.data() + offset)),
                                 chunk))
            {
                error = "VMMDLL_MemWrite failed at " + std::to_string(address + offset);
                return false;
            }
            offset += chunk;
        }
        return true;
    }

    bool virt2PhysAddress(VMM_HANDLE handle, std::uint32_t pid, std::uint64_t va, std::uint64_t& pa, std::string& error)
    {
        if (handle == nullptr || pid == 0)
        {
            error = "invalid virt2phys parameters";
            return false;
        }

        ULONG64 physical {};
        if (!VMMDLL_MemVirt2Phys(handle, pid, va, &physical))
        {
            error = "VMMDLL_MemVirt2Phys failed at " + std::to_string(va);
            return false;
        }
        pa = physical;
        return true;
    }

    std::optional<VirtualRegion>
    queryVmmVirtual(VMM_HANDLE handle, const VirtualQueryRequest& request, LookupStatus& status, std::string& error)
    {
        status = LookupStatus::Ok;
        if (handle == nullptr || request.pid == 0)
        {
            error = "invalid virtual query parameters";
            return std::nullopt;
        }
        if (requestExpired())
        {
            error = "request deadline exceeded";
            return std::nullopt;
        }

        // MemProcFS answers only for a page-aligned address, and an address inside a page belongs to the
        // region of that page.
        VMMDLL_MEMORY_BASIC_INFORMATION info {};
        const auto                      written =
            VMMDLL_MemVirtualQuery(handle, request.pid, request.address & ~kPageOffsetMask, &info, sizeof(info));
        if (written == 0)
        {
            status = LookupStatus::NotFound;
            return std::nullopt;
        }
        if (written < sizeof(info))
        {
            error = "VMMDLL_MemVirtualQuery returned a short region";
            return std::nullopt;
        }

        VirtualRegion region;
        region.baseAddress       = info.BaseAddress;
        region.allocationBase    = info.AllocationBase;
        region.regionSize        = info.RegionSize;
        region.allocationProtect = info.AllocationProtect;
        region.state             = info.State;
        region.protect           = info.Protect;
        region.type              = static_cast<std::uint32_t>(info.Type);
        region.partitionId       = info.PartitionId;
        return region;
    }

    bool readVmmPages(VMM_HANDLE                     handle,
                      std::uint32_t                  pid,
                      std::span<const std::uint64_t> addresses,
                      std::vector<PageReadItem>&     out,
                      std::string&                   error)
    {
        if (handle == nullptr || pid == 0 || addresses.empty())
        {
            error = "invalid page read parameters";
            return false;
        }

        out.clear();
        out.reserve(addresses.size());

        std::vector<std::byte> buffer(kPageSize);
        for (const auto address : addresses)
        {
            if (requestExpired())
            {
                error = "request deadline exceeded";
                return false;
            }

            PageReadItem item;
            item.address = address;
            item.ok      = VMMDLL_MemReadPage(handle, pid, address, reinterpret_cast<PBYTE>(buffer.data())) != 0;
            if (item.ok)
            {
                item.data = buffer;
            }
            out.push_back(std::move(item));
        }
        return true;
    }

    bool
    prefetchVmmPages(VMM_HANDLE handle, std::uint32_t pid, std::span<const std::uint64_t> addresses, std::string& error)
    {
        if (handle == nullptr || pid == 0 || addresses.empty())
        {
            error = "invalid prefetch parameters";
            return false;
        }

        // The call reports no per-page status: a page MemProcFS cannot cache simply stays uncached, which is
        // indistinguishable from a page it already had.
        constexpr std::size_t kPrefetchChunk = 1024;
        std::vector<ULONG64>  chunk;
        for (std::size_t offset = 0; offset < addresses.size(); offset += kPrefetchChunk)
        {
            if (requestExpired())
            {
                error = "request deadline exceeded";
                return false;
            }

            const auto count = std::min(kPrefetchChunk, addresses.size() - offset);
            chunk.assign(addresses.begin() + static_cast<std::ptrdiff_t>(offset),
                         addresses.begin() + static_cast<std::ptrdiff_t>(offset + count));
            if (!VMMDLL_MemPrefetchPages(handle, pid, chunk.data(), static_cast<DWORD>(chunk.size())))
            {
                error = "VMMDLL_MemPrefetchPages failed";
                return false;
            }
        }
        return true;
    }
} // namespace mpfshost
