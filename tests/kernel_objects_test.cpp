#include <catch2/catch.hpp>

#include "core/server/kernel_object_provider.hpp"
#include "core/server/map_batching.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

TEST_CASE("kernel object filter accepts everything when the request has no filter", "[core][kernelobjects]")
{
    mpfshost::KernelObjectDescriptor object;
    object.name = "\\Device\\HarddiskVolume1";

    REQUIRE(mpfshost::matchesKernelObjectFilter({""}, object));
}

TEST_CASE("kernel object filter matches the name and the type case-insensitively", "[core][kernelobjects]")
{
    mpfshost::KernelObjectDescriptor object;
    object.name = "\\Device\\HarddiskVolume1";
    object.type = "WindowStation";

    REQUIRE(mpfshost::matchesKernelObjectFilter({"device"}, object));
    REQUIRE(mpfshost::matchesKernelObjectFilter({"HARDDISKVOLUME1"}, object));
    REQUIRE(mpfshost::matchesKernelObjectFilter({"windowstation"}, object));
    REQUIRE_FALSE(mpfshost::matchesKernelObjectFilter({"absent"}, object));
}

TEST_CASE("kernel object encoded size matches the wire layout and the child cap", "[core][kernelobjects][batching]")
{
    mpfshost::KernelObjectDescriptor object;
    object.name       = "abc";
    object.type       = "de";
    object.childCount = 1000;
    for (std::uint64_t i = 0; i < mpfshost::kMaxKernelObjectChildren; ++i)
    {
        object.children.push_back(0x1000 + i);
    }
    object.childrenTruncated = true;

    mpfshost::KObjectListData data;
    data.objects.push_back(object);

    const auto encoded = mpfshost::encodeKObjectListData(data);
    REQUIRE(mpfshost::kernelObjectEncodedSize(object) == encoded.size() - sizeof(std::uint32_t));

    object.children.clear();
    REQUIRE(mpfshost::kernelObjectEncodedSize(object) == 37 + object.name.size() + object.type.size());
}

TEST_CASE("kernel object batch budget yields multiple frames for a large result", "[core][kernelobjects][batching]")
{
    std::vector<mpfshost::KernelObjectDescriptor> objects(1000);
    for (auto& object : objects)
    {
        object.name       = std::string(32, 'a');
        object.type       = std::string(32, 'b');
        object.childCount = mpfshost::kMaxKernelObjectChildren;
        object.children.assign(mpfshost::kMaxKernelObjectChildren, 0x1000);
        object.childrenTruncated = true;
    }

    std::size_t batches = 0;
    for (std::size_t offset = 0; offset < objects.size();)
    {
        const auto count =
            mpfshost::batchCount(objects, offset, mpfshost::kMaxMapBatchBytes, mpfshost::kernelObjectEncodedSize);
        REQUIRE(count > 0);
        offset += count;
        ++batches;
    }
    REQUIRE(batches > 1);
}
