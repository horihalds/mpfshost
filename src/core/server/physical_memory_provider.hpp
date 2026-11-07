#ifndef MPFSHOST_CORE_SERVER_PHYSICAL_MEMORY_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_PHYSICAL_MEMORY_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"
#include "core/server/scatter_provider.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mpfshost
{
    // Guest physical memory, addressed by GPA inside one of the partitions reported by VmProvider.
    class PhysicalMemoryProvider
    {
    public:
        virtual ~PhysicalMemoryProvider() = default;

        [[nodiscard]] virtual bool readPhysical(std::uint32_t           vmIndex,
                                                std::uint64_t           gpa,
                                                std::uint32_t           size,
                                                std::vector<std::byte>& out,
                                                std::string&            error)     = 0;
        [[nodiscard]] virtual bool writePhysical(std::uint32_t              vmIndex,
                                                 std::uint64_t              gpa,
                                                 std::span<const std::byte> data,
                                                 std::string&               error) = 0;
        [[nodiscard]] virtual bool
        translateGpa(std::uint32_t vmIndex, std::uint64_t gpa, GpaTranslateResponse& out, std::string& error) = 0;
        [[nodiscard]] virtual bool scatterPhysical(std::uint32_t                    vmIndex,
                                                   const std::vector<ScatterEntry>& entries,
                                                   std::vector<ScatterResult>&      results,
                                                   std::string&                     error)                    = 0;
    };

    // True when [gpa, gpa + size) stays inside the partition, which MemProcFS reports as gpaMax.
    [[nodiscard]] bool isWithinPhysicalRange(std::uint64_t gpa, std::uint64_t size, std::uint64_t gpaMax);

    // MEM_SCATTER accepts 8..0x1000 bytes, four-byte aligned and never crossing a page boundary.
    [[nodiscard]] bool isValidPhysicalScatterEntry(const ScatterEntry& entry);
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_PHYSICAL_MEMORY_PROVIDER_HPP
