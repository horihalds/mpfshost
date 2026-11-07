#include "host/vmm_pfn.hpp"

#include "core/ipc/frame.hpp"
#include "core/server/request_deadline.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mpfshost
{
    namespace
    {
        constexpr std::uint8_t kWriteInProgress  = 1U << 0;
        constexpr std::uint8_t kModified         = 1U << 1;
        constexpr std::uint8_t kReadInProgress   = 1U << 2;
        constexpr std::uint8_t kRemovalRequested = 1U << 3;
        constexpr std::uint8_t kParityError      = 1U << 4;
        constexpr std::uint8_t kInPageError      = 1U << 5;
        constexpr std::uint8_t kPrototypePte     = 1U << 6;
        constexpr std::uint8_t kKernelStack      = 1U << 7;
    } // namespace

    std::optional<PfnListResult>
    listVmmPfn(VMM_HANDLE handle, const PfnListRequest& request, LookupStatus& status, std::string& error)
    {
        (void)status;
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        const std::uint64_t count = request.pfnCount;
        if (count == 0 || count > kMaxPfnEntries
            || static_cast<std::uint64_t>(request.pfnStart) + count > 0x100000000ULL)
        {
            error = "PFN range is out of bounds";
            return std::nullopt;
        }
        if (requestExpired())
        {
            error = "request deadline exceeded";
            return std::nullopt;
        }

        std::vector<DWORD> pfns(static_cast<std::size_t>(count));
        for (std::size_t i = 0; i < pfns.size(); ++i)
        {
            pfns[i] = static_cast<DWORD>(request.pfnStart + i);
        }

        // VMMDLL_Map_GetPfn fills a caller buffer instead of allocating, so the buffer is sized for exactly the
        // requested entries and nothing has to be released with VMMDLL_MemFree. An eight-byte-aligned word vector
        // keeps the entry array aligned without a manual allocation.
        const std::size_t          bytes = sizeof(VMMDLL_MAP_PFN) + pfns.size() * sizeof(VMMDLL_MAP_PFNENTRY);
        constexpr std::size_t      kWord = sizeof(std::uint64_t);
        std::vector<std::uint64_t> storage((bytes + kWord - 1) / kWord, 0);
        auto*                      map = reinterpret_cast<PVMMDLL_MAP_PFN>(storage.data());
        map->dwVersion                 = VMMDLL_MAP_PFN_VERSION;
        DWORD bufferBytes              = static_cast<DWORD>(bytes);

        if (!VMMDLL_Map_GetPfn(handle, pfns.data(), static_cast<DWORD>(pfns.size()), map, &bufferBytes))
        {
            error = "VMMDLL_Map_GetPfn failed";
            return std::nullopt;
        }

        PfnListResult result;
        result.entries.reserve(map->cMap);
        for (DWORD i = 0; i < map->cMap; ++i)
        {
            const auto&   entry = map->pMap[i];
            PfnDescriptor frame;
            frame.pfn            = entry.dwPfn;
            frame.typeExtended   = static_cast<std::uint8_t>(entry.tpExtended);
            frame.pageLocation   = static_cast<std::uint8_t>(entry.PageLocation);
            frame.pid            = entry.AddressInfo.dwPid;
            frame.va             = entry.AddressInfo.va;
            frame.vaPte          = entry.vaPte;
            frame.originalPte    = entry.OriginalPte;
            frame.pteFrame       = entry.PteFrame;
            frame.referenceCount = entry.ReferenceCount;
            frame.cacheAttribute = entry.CacheAttribute;
            frame.priority       = entry.Priority;
            frame.pageColor      = entry.PageColor;

            std::uint8_t flags = 0;
            flags |= entry.WriteInProgress != 0 ? kWriteInProgress : 0;
            flags |= entry.Modified != 0 ? kModified : 0;
            flags |= entry.ReadInProgress != 0 ? kReadInProgress : 0;
            flags |= entry.RemovalRequested != 0 ? kRemovalRequested : 0;
            flags |= entry.ParityError != 0 ? kParityError : 0;
            flags |= entry.InPageError != 0 ? kInPageError : 0;
            flags |= entry.PrototypePte != 0 ? kPrototypePte : 0;
            flags |= entry.KernelStack_SystemChargedPage != 0 ? kKernelStack : 0;
            frame.flags = flags;

            result.entries.push_back(frame);
        }
        return result;
    }
} // namespace mpfshost
