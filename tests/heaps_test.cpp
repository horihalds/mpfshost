#include <catch2/catch.hpp>

#include "core/server/heap_alloc_provider.hpp"
#include "core/server/heap_provider.hpp"
#include "core/server/map_batching.hpp"
#include "support/fake_backend.hpp"

#include <string>
#include <vector>

namespace
{
    mpfshost::HeapDescriptor makeHeap(std::uint32_t index, std::uint32_t number)
    {
        mpfshost::HeapDescriptor heap;
        heap.address = 0x000001c000010000ull + index * 0x1000;
        heap.index   = index;
        heap.number  = number;
        heap.type    = 1;
        return heap;
    }
} // namespace

TEST_CASE("heap listing reaches the provider with the requested pid", "[core][heap]")
{
    mpfshost::testing::FakeBackend backend;
    mpfshost::VmTarget             target;
    target.name = "vm-alpha";

    std::string error;
    const auto  handle = backend.acquire(target, error);
    REQUIRE(handle != nullptr);

    auto* provider = dynamic_cast<mpfshost::HeapProvider*>(handle.get());
    REQUIRE(provider != nullptr);

    backend.heapState()->heaps = {makeHeap(0, 3), makeHeap(1, 9)};

    mpfshost::LookupStatus status = mpfshost::LookupStatus::Failed;
    const auto             result = provider->listHeaps({4321}, status, error);
    REQUIRE(result.has_value());
    REQUIRE(status == mpfshost::LookupStatus::Ok);
    REQUIRE(result->heaps.size() == 2);
    REQUIRE(result->heaps[0].number == 3);
    REQUIRE(result->heaps[1].index == 1);
    REQUIRE_FALSE(result->truncated);
    REQUIRE(backend.heapState()->lastRequest.has_value());
    REQUIRE(backend.heapState()->lastRequest->pid == 4321);
}

TEST_CASE("heap listing carries the truncation flag from the host", "[core][heap]")
{
    mpfshost::testing::FakeBackend backend;
    mpfshost::VmTarget             target;
    target.name = "vm-alpha";

    std::string error;
    const auto  handle = backend.acquire(target, error);
    REQUIRE(handle != nullptr);

    auto* provider = dynamic_cast<mpfshost::HeapProvider*>(handle.get());
    REQUIRE(provider != nullptr);

    backend.heapState()->heaps     = {makeHeap(0, 1)};
    backend.heapState()->truncated = true;

    mpfshost::LookupStatus status {};
    const auto             result = provider->listHeaps({4321}, status, error);
    REQUIRE(result.has_value());
    REQUIRE(result->truncated);
}

TEST_CASE("a vanished process answers NotFound for both heap listings", "[core][heap]")
{
    mpfshost::testing::FakeBackend backend;
    mpfshost::VmTarget             target;
    target.name = "vm-alpha";

    std::string error;
    const auto  handle = backend.acquire(target, error);
    REQUIRE(handle != nullptr);

    auto* heapProvider      = dynamic_cast<mpfshost::HeapProvider*>(handle.get());
    auto* heapAllocProvider = dynamic_cast<mpfshost::HeapAllocProvider*>(handle.get());
    REQUIRE(heapProvider != nullptr);
    REQUIRE(heapAllocProvider != nullptr);

    backend.heapState()->failure      = "process 4321 is not present";
    backend.heapState()->status       = mpfshost::LookupStatus::NotFound;
    backend.heapAllocState()->failure = "process 4321 is not present";
    backend.heapAllocState()->status  = mpfshost::LookupStatus::NotFound;

    mpfshost::LookupStatus status {};
    REQUIRE_FALSE(heapProvider->listHeaps({4321}, status, error).has_value());
    REQUIRE(status == mpfshost::LookupStatus::NotFound);

    status = mpfshost::LookupStatus::Ok;
    REQUIRE_FALSE(heapAllocProvider->listHeapAllocations({4321, 3}, status, error).has_value());
    REQUIRE(status == mpfshost::LookupStatus::NotFound);
}

TEST_CASE("heap allocation listing forwards the heap selector", "[core][heap]")
{
    mpfshost::testing::FakeBackend backend;
    mpfshost::VmTarget             target;
    target.name = "vm-alpha";

    std::string error;
    const auto  handle = backend.acquire(target, error);
    REQUIRE(handle != nullptr);

    auto* provider = dynamic_cast<mpfshost::HeapAllocProvider*>(handle.get());
    REQUIRE(provider != nullptr);

    mpfshost::HeapAllocDescriptor allocation;
    allocation.address                    = 0x000001c000020000ull;
    allocation.size                       = 0x1000;
    allocation.type                       = 5;
    backend.heapAllocState()->allocations = {allocation};

    mpfshost::LookupStatus status {};
    const auto             result = provider->listHeapAllocations({4321, 0x000001c000010000ull}, status, error);
    REQUIRE(result.has_value());
    REQUIRE(status == mpfshost::LookupStatus::Ok);
    REQUIRE(result->allocations.size() == 1);
    REQUIRE(result->allocations[0].size == 0x1000);
    REQUIRE(result->allocations[0].type == 5);
    REQUIRE(backend.heapAllocState()->lastRequest.has_value());
    REQUIRE(backend.heapAllocState()->lastRequest->pid == 4321);
    REQUIRE(backend.heapAllocState()->lastRequest->heap == 0x000001c000010000ull);
}

TEST_CASE("heap entry encoded sizes match the wire layout", "[core][heap][batching]")
{
    const auto heap = makeHeap(0, 1);
    REQUIRE(mpfshost::heapEncodedSize(heap) == 18);

    mpfshost::HeapAllocDescriptor allocation;
    allocation.address = 0x1000;
    allocation.size    = 0x20;
    allocation.type    = 1;
    REQUIRE(mpfshost::heapAllocEncodedSize(allocation) == 13);
}

TEST_CASE("heap allocation batches stay bounded by the frame budget", "[core][heap][batching]")
{
    std::vector<mpfshost::HeapAllocDescriptor> allocations(100000);
    std::size_t                                batches = 0;
    for (std::size_t offset = 0; offset < allocations.size();)
    {
        const auto count =
            mpfshost::batchCount(allocations, offset, mpfshost::kMaxMapBatchBytes, mpfshost::heapAllocEncodedSize);
        REQUIRE(count > 0);
        offset += count;
        ++batches;
    }
    REQUIRE(batches > 1);
}
