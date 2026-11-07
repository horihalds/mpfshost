#include "core/server/handle_cache.hpp"

#include "core/log/log.hpp"

#include <chrono>
#include <utility>
#include <vector>

namespace mpfshost
{
    namespace
    {
        bool sameTarget(const VmTarget& left, const VmTarget& right)
        {
            return left.qemuPid == right.qemuPid && left.qmpSocket == right.qmpSocket;
        }
    } // namespace

    CachingBackend::CachingBackend(VmBackend& inner, HandleCacheOptions options, Now now)
        : _inner {inner}, _options {options}, _now {std::move(now)}
    {
        if (_options.sweepInterval.count() > 0)
        {
            _sweeper = std::jthread {[this](std::stop_token stop)
                                     {
                                         runSweeper(stop);
                                     }};
        }
    }

    CachingBackend::~CachingBackend()
    {
        if (_sweeper.joinable())
        {
            _sweeper.request_stop();
            _sweepWake.notify_all();
            _sweeper.join();
        }

        // Release the handles outside the lock: closing a MemProcFS handle must never run while the cache is
        // locked, otherwise an eviction could deadlock against a session that is trying to acquire.
        std::unordered_map<std::string, Entry> entries;
        {
            std::lock_guard lock {_mutex};
            entries.swap(_entries);
        }
        log::debug("cache", "released ", entries.size(), " cached handle(s) at shutdown");
    }

    std::shared_ptr<VmHandle> CachingBackend::acquire(const VmTarget& target, std::string& error)
    {
        error.clear();
        sweep();

        std::promise<Creation> promise;
        Flight                 flight;
        bool                   creator = false;

        {
            std::lock_guard lock {_mutex};

            if (const auto found = _entries.find(target.name); found != _entries.end())
            {
                if (sameTarget(found->second.target, target))
                {
                    found->second.lastUsed = _now();
                    log::debug("cache", "reusing VMM handle for target ", target.name);
                    return found->second.handle;
                }

                log::info("cache", "target ", target.name, " now points at a different VM; dropping its handle");
                _entries.erase(found);
            }

            if (const auto pending = _inFlight.find(target.name); pending != _inFlight.end())
            {
                flight = pending->second;
            }
            else
            {
                flight = std::make_shared<std::shared_future<Creation>>(promise.get_future().share());
                _inFlight.emplace(target.name, flight);
                creator = true;
            }
        }

        if (!creator)
        {
            // Join the initialization another session is already running for the same cold target.
            const Creation creation = flight->get();
            error                   = creation.error;
            return creation.handle;
        }

        // A cold MemProcFS initialization takes hundreds of milliseconds, so it must run without the lock held
        // and unrelated targets must be able to initialize in parallel.
        std::shared_ptr<VmHandle> handle = _inner.acquire(target, error);

        {
            std::lock_guard lock {_mutex};
            _inFlight.erase(target.name);
            if (handle)
            {
                _entries.insert_or_assign(target.name, Entry {target, handle, _now()});
            }
        }

        promise.set_value(Creation {handle, error});
        return handle;
    }

    std::size_t CachingBackend::sweep()
    {
        if (_options.idleTimeout.count() <= 0)
        {
            return 0;
        }

        std::vector<std::pair<std::string, std::shared_ptr<VmHandle>>> released;
        {
            std::lock_guard lock {_mutex};
            const auto      now = _now();
            for (auto entry = _entries.begin(); entry != _entries.end();)
            {
                const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(now - entry->second.lastUsed);
                if (idle < _options.idleTimeout)
                {
                    ++entry;
                    continue;
                }

                log::info(
                    "cache", "releasing VMM handle for target ", entry->first, " after ", idle.count(), " ms idle");
                released.emplace_back(entry->first, std::move(entry->second.handle));
                entry = _entries.erase(entry);
            }
        }
        return released.size();
    }

    std::size_t CachingBackend::size() const
    {
        std::lock_guard lock {_mutex};
        return _entries.size();
    }

    void CachingBackend::runSweeper(std::stop_token stop)
    {
        std::unique_lock lock {_mutex};
        while (!stop.stop_requested())
        {
            _sweepWake.wait_for(lock,
                                stop,
                                _options.sweepInterval,
                                []
                                {
                                    return false;
                                });
            if (stop.stop_requested())
            {
                return;
            }

            lock.unlock();
            sweep();
            lock.lock();
        }
    }
} // namespace mpfshost
