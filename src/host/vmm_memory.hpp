#ifndef MPFSHOST_HOST_VMM_MEMORY_HPP
#define MPFSHOST_HOST_VMM_MEMORY_HPP

#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "vmmdll.h"

namespace mpfshost
{
    [[nodiscard]] bool readVmmMemory(VMM_HANDLE           handle,
                                     std::uint32_t        pid,
                                     std::uint64_t        address,
                                     std::span<std::byte> destination,
                                     std::string&         error);
    // Reads pages until the first one fails and returns the readable prefix. A completely unreadable range is a
    // successful empty result so callers can distinguish "unmapped" from "the request failed".
    [[nodiscard]] bool readVmmMemoryPartial(VMM_HANDLE              handle,
                                            std::uint32_t           pid,
                                            std::uint64_t           address,
                                            std::uint32_t           size,
                                            std::vector<std::byte>& out,
                                            std::string&            error);
    [[nodiscard]] bool writeVmmMemory(VMM_HANDLE                 handle,
                                      std::uint32_t              pid,
                                      std::uint64_t              address,
                                      std::span<const std::byte> source,
                                      std::string&               error);
    [[nodiscard]] bool
    virt2PhysAddress(VMM_HANDLE handle, std::uint32_t pid, std::uint64_t va, std::uint64_t& pa, std::string& error);
    // Nothing with status NotFound when the address belongs to no region; an error without NotFound means the
    // query itself failed.
    [[nodiscard]] std::optional<VirtualRegion>
    queryVmmVirtual(VMM_HANDLE handle, const VirtualQueryRequest& request, LookupStatus& status, std::string& error);
    // One item per address; a page MemProcFS cannot read is reported with ok = false instead of failing.
    [[nodiscard]] bool readVmmPages(VMM_HANDLE                     handle,
                                    std::uint32_t                  pid,
                                    std::span<const std::uint64_t> addresses,
                                    std::vector<PageReadItem>&     out,
                                    std::string&                   error);
    [[nodiscard]] bool prefetchVmmPages(VMM_HANDLE                     handle,
                                        std::uint32_t                  pid,
                                        std::span<const std::uint64_t> addresses,
                                        std::string&                   error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_MEMORY_HPP
