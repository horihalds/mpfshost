#ifndef MPFSHOST_CORE_SERVER_HANDLE_CACHE_HPP
#define MPFSHOST_CORE_SERVER_HANDLE_CACHE_HPP
#pragma once

#include "core/server/backend.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace mpfshost
{
    struct HandleCacheOptions
    {
        // How long a handle stays cached after the last session used it; zero keeps it until shutdown.
        std::chrono::milliseconds idleTimeout {60'000};
        // Sweeper period; zero disables the background thread and leaves eviction to acquire()/sweep().
        std::chrono::milliseconds sweepInterval {15'000};
    };

    // Keeps one handle per target alive between sessions so a repeated bind does not repay the expensive
    // MemProcFS initialization. Sessions still hold their own reference, so eviction only drops the cache's.
    class CachingBackend final : public VmBackend
    {
    public:
        using Clock = std::chrono::steady_clock;
        using Now   = std::function<Clock::time_point()>;

        explicit CachingBackend(VmBackend& inner, HandleCacheOptions options = {}, Now now = Clock::now);
        ~CachingBackend() override;

        CachingBackend(const CachingBackend&)            = delete;
        CachingBackend& operator=(const CachingBackend&) = delete;

        [[nodiscard]] std::shared_ptr<VmHandle> acquire(const VmTarget& target, std::string& error) override;

        // Releases every entry idle for longer than the timeout and returns how many were released.
        std::size_t               sweep();
        [[nodiscard]] std::size_t size() const;

    private:
        struct Entry
        {
            VmTarget                  target; // the pid/qmp pair the handle was created for
            std::shared_ptr<VmHandle> handle;
            Clock::time_point         lastUsed;
        };

        struct Creation
        {
            std::shared_ptr<VmHandle> handle;
            std::string               error;
        };

        using Flight = std::shared_ptr<std::shared_future<Creation>>;

        void runSweeper(std::stop_token stop);

        VmBackend&                              _inner;
        HandleCacheOptions                      _options;
        Now                                     _now;
        mutable std::mutex                      _mutex;
        std::unordered_map<std::string, Entry>  _entries;
        std::unordered_map<std::string, Flight> _inFlight;
        std::condition_variable_any             _sweepWake;
        std::jthread                            _sweeper;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_HANDLE_CACHE_HPP
