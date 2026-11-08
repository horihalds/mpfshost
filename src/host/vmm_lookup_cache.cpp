#include "host/vmm_lookup_cache.hpp"

#include "core/server/module_lookup_provider.hpp"
#include "core/server/process_info_provider.hpp"
#include "host/vmm_modules.hpp"

#include <algorithm>
#include <utility>

namespace mpfshost
{
    namespace
    {
        constexpr auto        kSnapshotTtl         = std::chrono::seconds(1);
        constexpr auto        kNegativeRefreshAge  = std::chrono::milliseconds(250);
        constexpr std::size_t kMaxCachedModulePids = 16;
    } // namespace

    LookupCache::ModuleView LookupCache::moduleSnapshot(std::uint32_t pid, bool force, std::string& error)
    {
        if (!force)
        {
            std::lock_guard lock {_mutex};
            const auto      found = _modules.find(pid);
            if (found != _modules.end() && Clock::now() - found->second.stamp < kSnapshotTtl)
            {
                return {found->second.modules, found->second.stamp, true};
            }
        }

        auto modules = listVmmModules(_handle, ModuleListRequest {pid, 0, {}}, error);
        if (!modules.has_value())
        {
            return {};
        }

        const auto      stamp  = Clock::now();
        auto            shared = std::make_shared<const std::vector<ModuleDescriptor>>(std::move(*modules));
        std::lock_guard lock {_mutex};
        if (_modules.size() >= kMaxCachedModulePids && _modules.find(pid) == _modules.end())
        {
            const auto oldest = std::min_element(_modules.begin(),
                                                 _modules.end(),
                                                 [](const auto& left, const auto& right)
                                                 {
                                                     return left.second.stamp < right.second.stamp;
                                                 });
            if (oldest != _modules.end())
            {
                _modules.erase(oldest);
            }
        }
        _modules[pid] = ModuleSnapshot {shared, stamp};
        return {std::move(shared), stamp, false};
    }

    LookupCache::ProcessView LookupCache::processSnapshot(bool force, std::string& error)
    {
        if (!force)
        {
            std::lock_guard lock {_mutex};
            if (_processes.has_value() && Clock::now() - _processes->stamp < kSnapshotTtl)
            {
                return {_processes->processes, _processes->stamp, true};
            }
        }

        auto processes = listVmmProcessDtbs(_handle, error);
        if (!processes.has_value())
        {
            return {};
        }

        const auto      stamp  = Clock::now();
        auto            shared = std::make_shared<const std::vector<ProcessDtbRecord>>(std::move(*processes));
        std::lock_guard lock {_mutex};
        _processes = ProcessSnapshot {shared, stamp};
        return {std::move(shared), stamp, false};
    }

    std::optional<ModuleByAddressResponse>
    LookupCache::moduleByAddress(const ModuleByAddressRequest& request, LookupStatus& status, std::string& error)
    {
        status = LookupStatus::Failed;
        if (_handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        auto view = moduleSnapshot(request.pid, false, error);
        if (view.modules == nullptr)
        {
            return std::nullopt;
        }

        const auto* module = findModuleAtAddress(*view.modules, request.address);
        if (module == nullptr && view.fromCache && Clock::now() - view.stamp >= kNegativeRefreshAge)
        {
            view = moduleSnapshot(request.pid, true, error);
            if (view.modules == nullptr)
            {
                return std::nullopt;
            }
            module = findModuleAtAddress(*view.modules, request.address);
        }

        if (module == nullptr)
        {
            status = LookupStatus::NotFound;
            error  = "address is not inside any module loaded in process " + std::to_string(request.pid);
            return std::nullopt;
        }

        ModuleByAddressResponse response;
        response.base      = module->base;
        response.entry     = module->entry;
        response.imageSize = module->imageSize;
        response.offset    = request.address - module->base;
        response.kind      = module->kind;
        response.wow64     = module->wow64;
        response.name      = module->name;
        response.fullName  = module->fullName;

        status = LookupStatus::Ok;
        return response;
    }

    std::optional<ProcessByDtbResponse>
    LookupCache::processByDtb(const ProcessByDtbRequest& request, LookupStatus& status, std::string& error)
    {
        status = LookupStatus::Failed;
        if (_handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        auto view = processSnapshot(false, error);
        if (view.processes == nullptr)
        {
            return std::nullopt;
        }

        const auto requested = canonicalDtb(request.dtb);
        auto       find      = [&](const ProcessView& snapshot, bool user) -> const ProcessDtbRecord*
        {
            for (const auto& record : *snapshot.processes)
            {
                const auto candidate = user ? canonicalDtb(record.dtbUser) : canonicalDtb(record.dtb);
                if (candidate != 0 && candidate == requested)
                {
                    return &record;
                }
            }
            return nullptr;
        };

        const ProcessDtbRecord* match       = find(view, false);
        bool                    matchedUser = false;
        if (match == nullptr)
        {
            match       = find(view, true);
            matchedUser = match != nullptr;
        }

        if (match == nullptr && view.fromCache && Clock::now() - view.stamp >= kNegativeRefreshAge)
        {
            view = processSnapshot(true, error);
            if (view.processes == nullptr)
            {
                return std::nullopt;
            }
            match = find(view, false);
            if (match == nullptr)
            {
                match       = find(view, true);
                matchedUser = match != nullptr;
            }
        }

        if (match == nullptr)
        {
            status = LookupStatus::NotFound;
            error  = "no process uses dtb " + std::to_string(requested);
            return std::nullopt;
        }

        ProcessByDtbResponse response;
        response.process     = match->process;
        response.dtb         = match->dtb;
        response.dtbUser     = match->dtbUser;
        response.matchedUser = matchedUser;
        status               = LookupStatus::Ok;
        return response;
    }
} // namespace mpfshost
