#include <catch2/catch.hpp>

#include "core/server/map_batching.hpp"
#include "core/server/thread_provider.hpp"

#include <string>
#include <vector>

namespace
{
    std::uint64_t threadFrameSize(const mpfshost::ThreadFrame& frame)
    {
        return 33 + frame.module.size() + frame.function.size();
    }

    std::uint64_t threadSize(const mpfshost::ThreadDescriptor& thread)
    {
        std::uint64_t size = 151;
        for (const auto& frame : thread.frames)
        {
            size += threadFrameSize(frame);
        }
        return size;
    }
} // namespace

TEST_CASE("thread filter accepts every thread when the request has no tid", "[core][thread]")
{
    mpfshost::ThreadDescriptor thread;
    thread.tid = 4321;
    thread.pid = 1000;

    REQUIRE(mpfshost::matchesThreadFilter({1000, 0, 0}, thread));
    REQUIRE(mpfshost::matchesThreadFilter({1000, 0, mpfshost::kThreadFlagCallstack}, thread));
}

TEST_CASE("thread filter matches exactly the requested tid", "[core][thread]")
{
    mpfshost::ThreadDescriptor first;
    first.tid = 4321;
    mpfshost::ThreadDescriptor second;
    second.tid = 4322;

    const mpfshost::ThreadListRequest request {1000, 4322, 0};
    REQUIRE_FALSE(mpfshost::matchesThreadFilter(request, first));
    REQUIRE(mpfshost::matchesThreadFilter(request, second));
}

TEST_CASE("thread batch budget yields multiple frames for a large result", "[core][batching]")
{
    std::vector<mpfshost::ThreadDescriptor> threads;
    for (int i = 0; i < 300; ++i)
    {
        mpfshost::ThreadDescriptor thread;
        mpfshost::ThreadFrame      frame;
        frame.module   = std::string(2048, 'a');
        frame.function = std::string(2048, 'b');
        thread.frames.push_back(std::move(frame));
        threads.push_back(std::move(thread));
    }

    std::size_t batches = 0;
    for (std::size_t offset = 0; offset < threads.size();)
    {
        const auto count = mpfshost::batchCount(threads, offset, mpfshost::kMaxMapBatchBytes, threadSize);
        REQUIRE(count > 0);
        offset += count;
        ++batches;
    }
    REQUIRE(batches > 1);
}

TEST_CASE("thread batch budget still advances for an oversized thread", "[core][batching]")
{
    mpfshost::ThreadDescriptor thread;
    mpfshost::ThreadFrame      frame;
    frame.module   = std::string(300000, 'a');
    frame.function = std::string(300000, 'b');
    thread.frames.push_back(std::move(frame));
    const std::vector<mpfshost::ThreadDescriptor> threads {thread};

    REQUIRE(threadSize(thread) > mpfshost::kMaxMapBatchBytes);
    REQUIRE(mpfshost::batchCount(threads, 0, mpfshost::kMaxMapBatchBytes, threadSize) == 1);
}
