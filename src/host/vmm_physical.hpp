#ifndef MPFSHOST_HOST_VMM_PHYSICAL_HPP
#define MPFSHOST_HOST_VMM_PHYSICAL_HPP

#pragma once

#include "core/ipc/message.hpp"
#include "core/server/scatter_provider.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "vmmdll.h"

namespace mpfshost
{
    [[nodiscard]] bool readVmMemory(VMM_HANDLE           handle,
                                    std::uint32_t        vmIndex,
                                    std::uint64_t        gpa,
                                    std::span<std::byte> destination,
                                    std::string&         error);
    [[nodiscard]] bool writeVmMemory(VMM_HANDLE                 handle,
                                     std::uint32_t              vmIndex,
                                     std::uint64_t              gpa,
                                     std::span<const std::byte> source,
                                     std::string&               error);
    [[nodiscard]] bool translateVmGpa(
        VMM_HANDLE handle, std::uint32_t vmIndex, std::uint64_t gpa, GpaTranslateResponse& out, std::string& error);
    [[nodiscard]] bool scatterVmMemory(VMM_HANDLE                       handle,
                                       std::uint32_t                    vmIndex,
                                       const std::vector<ScatterEntry>& entries,
                                       std::vector<ScatterResult>&      results,
                                       std::string&                     error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_PHYSICAL_HPP
