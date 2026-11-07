#ifndef MPFSHOST_CORE_SERVER_REQUEST_DEADLINE_HPP
#define MPFSHOST_CORE_SERVER_REQUEST_DEADLINE_HPP
#pragma once

#include <chrono>

namespace mpfshost
{
    // A MemProcFS call cannot be preempted, so the budget is polled only where stopping is safe.
    class RequestDeadline
    {
    public:
        void                                    restart(std::chrono::milliseconds budget) noexcept;
        [[nodiscard]] bool                      expired() const noexcept;
        [[nodiscard]] std::chrono::milliseconds remaining() const noexcept;

    private:
        std::chrono::steady_clock::time_point _expiry {};
        bool                                  _enabled {false};
    };

    // Publishes the running request's budget to the host without threading a parameter through every provider.
    class RequestDeadlineScope
    {
    public:
        explicit RequestDeadlineScope(const RequestDeadline& deadline) noexcept;
        ~RequestDeadlineScope();

        RequestDeadlineScope(const RequestDeadlineScope&)            = delete;
        RequestDeadlineScope& operator=(const RequestDeadlineScope&) = delete;

    private:
        const RequestDeadline* _previous;
    };

    [[nodiscard]] bool requestExpired() noexcept;
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_REQUEST_DEADLINE_HPP
