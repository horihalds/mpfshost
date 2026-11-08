#ifndef MPFSHOST_HOST_VMM_LOOKUP_CACHE_HPP
#define MPFSHOST_HOST_VMM_LOOKUP_CACHE_HPP
#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"
#include "host/vmm_processes.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "vmmdll.h"

namespace mpfshost
{
    // Short-lived snapshots behind the two address lookups. Watchpoint symbolization tends to ask for several
    // addresses in a row, while a process or DLL can appear at any moment, so entries expire quickly and a stale
    // miss is retried once before it is reported as NotFound.
    class LookupCache
    {
    public:
        explicit LookupCache(VMM_HANDLE handle) : _handle {handle} {}

        LookupCache(const LookupCache&)            = delete;
        LookupCache& operator=(const LookupCache&) = delete;

        [[nodiscard]] std::optional<ModuleByAddressResponse>
        moduleByAddress(const ModuleByAddressRequest& request, LookupStatus& status, std::string& error);

        [[nodiscard]] std::optional<ProcessByDtbResponse>
        processByDtb(const ProcessByDtbRequest& request, LookupStatus& status, std::string& error);

    private:
        using Clock = std::chrono::steady_clock;

        struct ModuleSnapshot
        {
            std::shared_ptr<const std::vector<ModuleDescriptor>> modules;
            Clock::time_point                                    stamp;
        };

        struct ProcessSnapshot
        {
            std::shared_ptr<const std::vector<ProcessDtbRecord>> processes;
            Clock::time_point                                    stamp;
        };

        struct ModuleView
        {
            std::shared_ptr<const std::vector<ModuleDescriptor>> modules;
            Clock::time_point                                    stamp;
            bool                                                 fromCache {};
        };

        struct ProcessView
        {
            std::shared_ptr<const std::vector<ProcessDtbRecord>> processes;
            Clock::time_point                                    stamp;
            bool                                                 fromCache {};
        };

        [[nodiscard]] ModuleView  moduleSnapshot(std::uint32_t pid, bool force, std::string& error);
        [[nodiscard]] ProcessView processSnapshot(bool force, std::string& error);

        VMM_HANDLE                                        _handle;
        mutable std::mutex                                _mutex;
        std::unordered_map<std::uint32_t, ModuleSnapshot> _modules;
        std::optional<ProcessSnapshot>                    _processes;
    };
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_LOOKUP_CACHE_HPP
