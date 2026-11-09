#include "core/server/request_deadline.hpp"

namespace mpfshost
{
    namespace
    {
        thread_local const RequestDeadline* activeDeadline = nullptr;
    } // namespace

    void RequestDeadline::restart(std::chrono::milliseconds budget) noexcept
    {
        _enabled = budget.count() > 0;
        _expiry  = _enabled ? std::chrono::steady_clock::now() + budget : std::chrono::steady_clock::time_point {};
    }

    bool RequestDeadline::expired() const noexcept
    {
        return _enabled && std::chrono::steady_clock::now() >= _expiry;
    }

    std::chrono::milliseconds RequestDeadline::remaining() const noexcept
    {
        if (!_enabled)
        {
            return std::chrono::milliseconds::zero();
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= _expiry)
        {
            return std::chrono::milliseconds::zero();
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(_expiry - now);
    }

    RequestDeadlineScope::RequestDeadlineScope(const RequestDeadline& deadline) noexcept : _previous {activeDeadline}
    {
        activeDeadline = &deadline;
    }

    RequestDeadlineScope::~RequestDeadlineScope()
    {
        activeDeadline = _previous;
    }

    bool requestExpired() noexcept
    {
        return activeDeadline != nullptr && activeDeadline->expired();
    }

    std::chrono::milliseconds requestRemaining(std::chrono::milliseconds fallback) noexcept
    {
        if (activeDeadline == nullptr)
        {
            return fallback;
        }
        return activeDeadline->remaining();
    }
} // namespace mpfshost
