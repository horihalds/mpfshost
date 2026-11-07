#include <catch2/catch.hpp>

#include "core/server/kernel_device_provider.hpp"
#include "core/server/map_batching.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

TEST_CASE("kernel device filter accepts everything when the request has no filter", "[core][kerneldevices]")
{
    mpfshost::KernelDeviceDescriptor device;
    device.deviceTypeName = "FILE_DEVICE_DISK";

    REQUIRE(mpfshost::matchesKernelDeviceFilter({""}, device));
}

TEST_CASE("kernel device filter matches the type name and the volume info case-insensitively", "[core][kerneldevices]")
{
    mpfshost::KernelDeviceDescriptor device;
    device.deviceTypeName = "FILE_DEVICE_DISK";
    device.volumeInfo     = "C:";

    REQUIRE(mpfshost::matchesKernelDeviceFilter({"file_device_disk"}, device));
    REQUIRE(mpfshost::matchesKernelDeviceFilter({"DISK"}, device));
    REQUIRE(mpfshost::matchesKernelDeviceFilter({"c:"}, device));
    REQUIRE_FALSE(mpfshost::matchesKernelDeviceFilter({"absent"}, device));
}

TEST_CASE("kernel device encoded size matches the wire layout", "[core][kerneldevices][batching]")
{
    mpfshost::KernelDeviceDescriptor device;
    device.deviceTypeName = "FILE_DEVICE_DISK";
    device.volumeInfo     = "C:";

    mpfshost::KDeviceListData data;
    data.devices.push_back(device);

    const auto encoded = mpfshost::encodeKDeviceListData(data);
    REQUIRE(mpfshost::kernelDeviceEncodedSize(device) == encoded.size() - sizeof(std::uint32_t));
    REQUIRE(mpfshost::kernelDeviceEncodedSize(device) == 48 + device.deviceTypeName.size() + device.volumeInfo.size());
}

TEST_CASE("kernel device batch budget yields multiple frames for a large result", "[core][kerneldevices][batching]")
{
    std::vector<mpfshost::KernelDeviceDescriptor> devices(20000);
    for (auto& device : devices)
    {
        device.deviceTypeName = std::string(64, 'a');
        device.volumeInfo     = std::string(64, 'b');
    }

    std::size_t batches = 0;
    for (std::size_t offset = 0; offset < devices.size();)
    {
        const auto count =
            mpfshost::batchCount(devices, offset, mpfshost::kMaxMapBatchBytes, mpfshost::kernelDeviceEncodedSize);
        REQUIRE(count > 0);
        offset += count;
        ++batches;
    }
    REQUIRE(batches > 1);
}
