#include <catch2/catch.hpp>

#include "core/server/handle_cache.hpp"
#include "support/fake_backend.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace
{
    using Milliseconds = std::chrono::milliseconds;
    using TimePoint    = mpfshost::CachingBackend::Clock::time_point;

    // Injectable, thread-safe clock so eviction can be driven without waiting for the real timeout.
    class TestClock
    {
    public:
        [[nodiscard]] TimePoint now() const
        {
            return TimePoint {Milliseconds {_millis.load(std::memory_order_relaxed)}};
        }

        void advance(Milliseconds delta)
        {
            _millis.fetch_add(delta.count(), std::memory_order_relaxed);
        }

    private:
        std::atomic<long long> _millis {};
    };

    mpfshost::HandleCacheOptions cacheOptions(Milliseconds idleTimeout)
    {
        mpfshost::HandleCacheOptions options;
        options.idleTimeout   = idleTimeout;
        options.sweepInterval = Milliseconds {0}; // no background thread: the tests drive sweep() themselves
        return options;
    }

    mpfshost::VmTarget makeTarget(std::string name, std::uint32_t pid)
    {
        return mpfshost::VmTarget {std::move(name), pid, "/var/qmp-sockets/" + name + ".sock"};
    }

    mpfshost::CachingBackend::Now clockNow(const TestClock& clock)
    {
        return [&clock]
        {
            return clock.now();
        };
    }
} // namespace

TEST_CASE("cache reuses one handle across sequential acquires", "[cache]")
{
    mpfshost::testing::CountingBackend inner;
    TestClock                          clock;
    mpfshost::CachingBackend           cache {inner, cacheOptions(Milliseconds {5'000}), clockNow(clock)};
    const auto                         target = makeTarget("vm-alpha", 111);

    std::string error;
    const auto  first = cache.acquire(target, error);
    REQUIRE(first != nullptr);
    REQUIRE(error.empty());
    REQUIRE(cache.size() == 1);

    const auto second = cache.acquire(target, error);
    REQUIRE(second == first);
    REQUIRE(inner.acquireCount() == 1);
    REQUIRE(cache.size() == 1);
}

TEST_CASE("cache evicts a handle once it has been idle for the timeout", "[cache]")
{
    mpfshost::testing::CountingBackend inner;
    TestClock                          clock;
    mpfshost::CachingBackend           cache {inner, cacheOptions(Milliseconds {5'000}), clockNow(clock)};
    const auto                         target = makeTarget("vm-alpha", 111);

    std::string                             error;
    auto                                    first = cache.acquire(target, error);
    const std::weak_ptr<mpfshost::VmHandle> weak {first};
    REQUIRE(first != nullptr);

    clock.advance(Milliseconds {4'000});
    REQUIRE(cache.sweep() == 0);
    REQUIRE(cache.size() == 1);

    clock.advance(Milliseconds {2'000});
    REQUIRE(cache.sweep() == 1);
    REQUIRE(cache.size() == 0);
    REQUIRE(inner.liveHandleCount() == 1); // the test still holds the handle

    first.reset();
    REQUIRE(weak.expired());

    const auto recreated = cache.acquire(target, error);
    REQUIRE(recreated != nullptr);
    REQUIRE(inner.acquireCount() == 2);
}

TEST_CASE("cache keeps handles forever when the idle timeout is zero", "[cache]")
{
    mpfshost::testing::CountingBackend inner;
    TestClock                          clock;
    mpfshost::CachingBackend           cache {inner, cacheOptions(Milliseconds {0}), clockNow(clock)};
    const auto                         target = makeTarget("vm-alpha", 111);

    std::string error;
    const auto  first = cache.acquire(target, error);
    REQUIRE(first != nullptr);

    clock.advance(Milliseconds {24 * 60 * 60 * 1'000});
    REQUIRE(cache.sweep() == 0);
    REQUIRE(cache.size() == 1);

    REQUIRE(cache.acquire(target, error) == first);
    REQUIRE(inner.acquireCount() == 1);
}

TEST_CASE("cache replaces a handle when the target now points at another VM", "[cache]")
{
    mpfshost::testing::CountingBackend inner;
    TestClock                          clock;
    mpfshost::CachingBackend           cache {inner, cacheOptions(Milliseconds {60'000}), clockNow(clock)};

    std::string                             error;
    auto                                    first = cache.acquire(makeTarget("vm-alpha", 111), error);
    const std::weak_ptr<mpfshost::VmHandle> weak {first};
    REQUIRE(first != nullptr);

    const auto second = cache.acquire(makeTarget("vm-alpha", 222), error);
    REQUIRE(second != nullptr);
    REQUIRE(second != first);
    REQUIRE(inner.acquireCount() == 2);
    REQUIRE(inner.lastTarget().qemuPid == 222);

    first.reset();
    REQUIRE(weak.expired());
}

TEST_CASE("cache tracks each target independently", "[cache]")
{
    mpfshost::testing::CountingBackend inner;
    TestClock                          clock;
    mpfshost::CachingBackend           cache {inner, cacheOptions(Milliseconds {5'000}), clockNow(clock)};
    const auto                         alpha = makeTarget("vm-alpha", 111);
    const auto                         beta  = makeTarget("vm-beta", 222);

    std::string error;
    const auto  alphaHandle = cache.acquire(alpha, error);
    REQUIRE(alphaHandle != nullptr);
    REQUIRE(cache.size() == 1);

    clock.advance(Milliseconds {2'000});
    const auto betaHandle = cache.acquire(beta, error);
    REQUIRE(betaHandle != nullptr);
    REQUIRE(betaHandle != alphaHandle);
    REQUIRE(cache.size() == 2);

    clock.advance(Milliseconds {2'000});
    REQUIRE(cache.acquire(alpha, error) == alphaHandle); // refreshes alpha only
    REQUIRE(inner.acquireCount() == 2);

    clock.advance(Milliseconds {2'000});
    REQUIRE(cache.sweep() == 0);
    REQUIRE(cache.size() == 2);

    clock.advance(Milliseconds {2'000});
    REQUIRE(cache.sweep() == 1); // beta has been idle for 6s, alpha for 4s
    REQUIRE(cache.size() == 1);

    REQUIRE(cache.acquire(beta, error) != nullptr);
    REQUIRE(inner.acquireCount() == 3);
}

TEST_CASE("cache does not keep a failed acquire", "[cache]")
{
    mpfshost::testing::CountingBackend inner;
    TestClock                          clock;
    mpfshost::CachingBackend           cache {inner, cacheOptions(Milliseconds {60'000}), clockNow(clock)};
    const auto                         target = makeTarget("vm-alpha", 111);

    inner.failWith("VMMDLL_Initialize failed");
    std::string error;
    REQUIRE(cache.acquire(target, error) == nullptr);
    REQUIRE(error == "VMMDLL_Initialize failed");
    REQUIRE(cache.size() == 0);

    inner.clearFailure();
    REQUIRE(cache.acquire(target, error) != nullptr);
    REQUIRE(error.empty());
    REQUIRE(inner.acquireCount() == 2);
    REQUIRE(cache.size() == 1);
}

TEST_CASE("cache joins concurrent acquires of the same cold target", "[cache]")
{
    mpfshost::testing::CountingBackend inner;
    TestClock                          clock;
    mpfshost::CachingBackend           cache {inner, cacheOptions(Milliseconds {60'000}), clockNow(clock)};
    const auto                         target = makeTarget("vm-alpha", 111);

    inner.blockAcquires();

    std::shared_ptr<mpfshost::VmHandle> first;
    std::thread                         firstThread {[&]
                                                     {
                                 std::string error;
                                 first = cache.acquire(target, error);
                                                     }};
    REQUIRE(inner.waitForAcquires(1, Milliseconds {2'000}));

    std::shared_ptr<mpfshost::VmHandle> second;
    std::thread                         secondThread {[&]
                                                      {
                                  std::string error;
                                  second = cache.acquire(target, error);
                                                      }};
    std::this_thread::sleep_for(Milliseconds {100});

    inner.releaseAcquires();
    firstThread.join();
    secondThread.join();

    REQUIRE(first != nullptr);
    REQUIRE(second == first);
    REQUIRE(inner.acquireCount() == 1);
    REQUIRE(cache.size() == 1);
}

TEST_CASE("cache initializes different targets in parallel", "[cache]")
{
    mpfshost::testing::CountingBackend inner;
    TestClock                          clock;
    mpfshost::CachingBackend           cache {inner, cacheOptions(Milliseconds {60'000}), clockNow(clock)};

    inner.blockAcquires();

    std::shared_ptr<mpfshost::VmHandle> alpha;
    std::thread                         firstThread {[&]
                                                     {
                                 std::string error;
                                 alpha = cache.acquire(makeTarget("vm-alpha", 111), error);
                                                     }};
    REQUIRE(inner.waitForAcquires(1, Milliseconds {2'000}));

    std::shared_ptr<mpfshost::VmHandle> beta;
    std::thread                         secondThread {[&]
                                                      {
                                  std::string error;
                                  beta = cache.acquire(makeTarget("vm-beta", 222), error);
                                                      }};
    // The second target must reach the inner backend while the first one is still inside its initialization.
    REQUIRE(inner.waitForAcquires(2, Milliseconds {2'000}));

    inner.releaseAcquires();
    firstThread.join();
    secondThread.join();

    REQUIRE(cache.size() == 2);
}

TEST_CASE("cache releases its handles when it is destroyed", "[cache]")
{
    mpfshost::testing::CountingBackend inner;
    TestClock                          clock;
    const auto                         target = makeTarget("vm-alpha", 111);

    std::string                         error;
    std::shared_ptr<mpfshost::VmHandle> sessionHandle;
    std::weak_ptr<mpfshost::VmHandle>   weak;
    {
        mpfshost::CachingBackend cache {inner, cacheOptions(Milliseconds {60'000}), clockNow(clock)};
        sessionHandle = cache.acquire(target, error);
        REQUIRE(sessionHandle != nullptr);
        weak = sessionHandle;
        REQUIRE(inner.liveHandleCount() == 1);
    }

    REQUIRE(inner.liveHandleCount() == 1); // a bound session keeps the handle usable
    REQUIRE_FALSE(weak.expired());

    sessionHandle.reset();
    REQUIRE(weak.expired());
    REQUIRE(inner.liveHandleCount() == 0);
}

TEST_CASE("cache sweeps idle handles from its background thread", "[cache]")
{
    mpfshost::testing::CountingBackend inner;
    TestClock                          clock;
    mpfshost::CachingBackend           cache {
        inner, {Milliseconds {1'000}, Milliseconds {10}},
         clockNow(clock)
    };

    std::string error;
    REQUIRE(cache.acquire(makeTarget("vm-alpha", 111), error) != nullptr);
    REQUIRE(cache.size() == 1);

    clock.advance(Milliseconds {2'000});

    bool released = false;
    for (int attempt = 0; attempt < 200 && !released; ++attempt)
    {
        released = cache.size() == 0;
        if (!released)
        {
            std::this_thread::sleep_for(Milliseconds {5});
        }
    }
    REQUIRE(released);
}
