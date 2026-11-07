#include "host/vmm_physical.hpp"

#include "core/server/request_deadline.hpp"
#include "host/vmm_vm.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>

namespace mpfshost
{
    namespace
    {
        constexpr std::size_t kMaxTransferChunk = 128 * 0x1000;

        // MemProcFS addresses the target's own physical memory as dwPID = (DWORD)-1; kPhysicalPid in frame.hpp
        // carries the same value for the requests that take a pid.

        // MEM_SCATTER arrays belong to LeechCore, so they are released through LcMemFree rather than delete.
        struct LcScatterArray
        {
            PPMEM_SCATTER memory {};
            DWORD         count {};

            LcScatterArray() = default;

            ~LcScatterArray()
            {
                if (memory != nullptr)
                {
                    LcMemFree(memory);
                }
            }

            LcScatterArray(const LcScatterArray&)            = delete;
            LcScatterArray& operator=(const LcScatterArray&) = delete;

            [[nodiscard]] bool allocate(std::size_t size, std::string& error)
            {
                count = static_cast<DWORD>(size);
                if (!LcAllocScatter1(count, &memory))
                {
                    memory = nullptr;
                    error  = "LcAllocScatter1 failed";
                    return false;
                }
                return true;
            }
        };
    } // namespace

    bool readVmMemory(VMM_HANDLE           handle,
                      std::uint32_t        vmIndex,
                      std::uint64_t        gpa,
                      std::span<std::byte> destination,
                      std::string&         error)
    {
        if (handle == nullptr || destination.empty())
        {
            error = "invalid guest memory read parameters";
            return false;
        }

        const auto resolution = resolveVmmVm(handle, vmIndex, error);
        if (!resolution.has_value())
        {
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
            const auto chunk  = static_cast<DWORD>(std::min(kMaxTransferChunk, destination.size() - offset));
            auto*      target = reinterpret_cast<PBYTE>(destination.data() + offset);
            if (resolution->vm == nullptr)
            {
                DWORD bytesRead = 0;
                if (!VMMDLL_MemReadEx(handle, kPhysicalPid, gpa + offset, target, chunk, &bytesRead, 0)
                    || bytesRead != chunk)
                {
                    error = "VMMDLL_MemReadEx failed at " + std::to_string(gpa + offset);
                    return false;
                }
            }
            else if (!VMMDLL_VmMemRead(handle, resolution->vm, gpa + offset, target, chunk))
            {
                error = "VMMDLL_VmMemRead failed at " + std::to_string(gpa + offset);
                return false;
            }
            offset += chunk;
        }
        return true;
    }

    bool writeVmMemory(VMM_HANDLE                 handle,
                       std::uint32_t              vmIndex,
                       std::uint64_t              gpa,
                       std::span<const std::byte> source,
                       std::string&               error)
    {
        if (handle == nullptr || source.empty())
        {
            error = "invalid guest memory write parameters";
            return false;
        }

        const auto resolution = resolveVmmVm(handle, vmIndex, error);
        if (!resolution.has_value())
        {
            return false;
        }

        const bool physical = resolution->vm == nullptr;

        std::size_t offset = 0;
        while (offset < source.size())
        {
            if (requestExpired())
            {
                error = "request deadline exceeded";
                return false;
            }
            const auto chunk  = static_cast<DWORD>(std::min(kMaxTransferChunk, source.size() - offset));
            auto*      target = reinterpret_cast<PBYTE>(const_cast<std::byte*>(source.data() + offset));
            const BOOL ok     = physical ? VMMDLL_MemWrite(handle, kPhysicalPid, gpa + offset, target, chunk)
                                         : VMMDLL_VmMemWrite(handle, resolution->vm, gpa + offset, target, chunk);
            if (!ok)
            {
                error = std::string {physical ? "VMMDLL_MemWrite failed at " : "VMMDLL_VmMemWrite failed at "}
                      + std::to_string(gpa + offset);
                return false;
            }
            offset += chunk;
        }
        return true;
    }

    bool translateVmGpa(
        VMM_HANDLE handle, std::uint32_t vmIndex, std::uint64_t gpa, GpaTranslateResponse& out, std::string& error)
    {
        if (handle == nullptr)
        {
            error = "VM handle is closed";
            return false;
        }

        const auto resolution = resolveVmmVm(handle, vmIndex, error);
        if (!resolution.has_value())
        {
            return false;
        }

        if (resolution->vm == nullptr)
        {
            // A GPA of the bound target is already a physical address, and no 'vmmem' mapping exists for it.
            out.pa        = gpa;
            out.paPresent = true;
            out.vaPresent = false;
            return true;
        }

        ULONG64    pa {};
        ULONG64    va {};
        const auto ok = VMMDLL_VmMemTranslateGPA(handle, resolution->vm, gpa, &pa, &va);
        if (!ok)
        {
            error = "VMMDLL_VmMemTranslateGPA failed at " + std::to_string(gpa);
            return false;
        }

        // MemProcFS leaves the untranslatable side zeroed; a zero address is reported as absent.
        out.pa        = pa;
        out.va        = va;
        out.paPresent = pa != 0;
        out.vaPresent = va != 0;
        return true;
    }

    bool scatterVmMemory(VMM_HANDLE                       handle,
                         std::uint32_t                    vmIndex,
                         const std::vector<ScatterEntry>& entries,
                         std::vector<ScatterResult>&      results,
                         std::string&                     error)
    {
        if (handle == nullptr || entries.empty())
        {
            error = "invalid guest scatter parameters";
            return false;
        }

        const auto resolution = resolveVmmVm(handle, vmIndex, error);
        if (!resolution.has_value())
        {
            return false;
        }

        std::vector<std::uint32_t> readIndices;
        std::vector<std::uint32_t> writeIndices;
        for (std::size_t index = 0; index < entries.size(); ++index)
        {
            auto& target = entries[index].kind == kScatterEntryWrite ? writeIndices : readIndices;
            target.push_back(static_cast<std::uint32_t>(index));
        }

        if (requestExpired())
        {
            error = "request deadline exceeded";
            return false;
        }

        results.assign(entries.size(), {});
        for (std::size_t index = 0; index < entries.size(); ++index)
        {
            results[index].address       = entries[index].address;
            results[index].requestedSize = entries[index].size;
        }

        if (!readIndices.empty())
        {
            LcScatterArray array;
            if (!array.allocate(readIndices.size(), error))
            {
                return false;
            }
            for (std::size_t slot = 0; slot < readIndices.size(); ++slot)
            {
                const auto& entry       = entries[readIndices[slot]];
                array.memory[slot]->qwA = entry.address;
                array.memory[slot]->cb  = entry.size;
            }

            if (resolution->vm == nullptr)
            {
                VMMDLL_MemReadScatter(handle, kPhysicalPid, array.memory, array.count, 0);
            }
            else
            {
                VMMDLL_VmMemReadScatter(handle, resolution->vm, array.memory, array.count, 0);
            }
            for (std::size_t slot = 0; slot < readIndices.size(); ++slot)
            {
                auto&       result = results[readIndices[slot]];
                const auto& memory = array.memory[slot];
                if (memory->f == 0)
                {
                    continue;
                }

                const auto available = std::min<std::size_t>(memory->cb, result.requestedSize);
                result.data.assign(reinterpret_cast<const std::byte*>(memory->pb),
                                   reinterpret_cast<const std::byte*>(memory->pb) + available);
                result.bytesRead = static_cast<std::uint32_t>(available);
                result.ok        = available == result.requestedSize;
            }
        }

        if (!writeIndices.empty())
        {
            LcScatterArray array;
            if (!array.allocate(writeIndices.size(), error))
            {
                return false;
            }
            for (std::size_t slot = 0; slot < writeIndices.size(); ++slot)
            {
                const auto& entry       = entries[writeIndices[slot]];
                array.memory[slot]->qwA = entry.address;
                array.memory[slot]->cb  = entry.size;
                std::memcpy(array.memory[slot]->pb, entry.data.data(), entry.size);
            }

            const auto written = resolution->vm == nullptr
                                   ? VMMDLL_MemWriteScatter(handle, kPhysicalPid, array.memory, array.count)
                                   : VMMDLL_VmMemWriteScatter(handle, resolution->vm, array.memory, array.count);
            for (const auto index : writeIndices)
            {
                auto&      result = results[index];
                const bool ok     = written == array.count;
                result.ok         = ok;
                result.bytesRead  = ok ? result.requestedSize : 0;
            }
        }

        return true;
    }
} // namespace mpfshost
