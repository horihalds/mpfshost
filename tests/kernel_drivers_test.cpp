#include <catch2/catch.hpp>

#include "core/server/kernel_driver_provider.hpp"
#include "core/server/map_batching.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

TEST_CASE("kernel driver filter accepts everything when the request has no filter", "[core][kerneldrivers]")
{
    mpfshost::KernelDriverDescriptor driver;
    driver.name = "Ntfs";

    REQUIRE(mpfshost::matchesKernelDriverFilter({""}, driver));
}

TEST_CASE("kernel driver filter matches name, path and service key case-insensitively", "[core][kerneldrivers]")
{
    mpfshost::KernelDriverDescriptor driver;
    driver.name           = "Ntfs";
    driver.path           = "\\SystemRoot\\System32\\drivers\\ntfs.sys";
    driver.serviceKeyName = "NtfsService";

    REQUIRE(mpfshost::matchesKernelDriverFilter({"ntfs"}, driver));
    REQUIRE(mpfshost::matchesKernelDriverFilter({"NTFS.SYS"}, driver));
    REQUIRE(mpfshost::matchesKernelDriverFilter({"drivers\\"}, driver));
    REQUIRE(mpfshost::matchesKernelDriverFilter({"ntfsservice"}, driver));
    REQUIRE_FALSE(mpfshost::matchesKernelDriverFilter({"absent"}, driver));
}

TEST_CASE("kernel driver encoded size matches the wire layout", "[core][kerneldrivers][batching]")
{
    mpfshost::KernelDriverDescriptor driver;
    driver.name           = "Ntfs";
    driver.path           = "C:\\ntfs.sys";
    driver.serviceKeyName = "Ntfs";
    driver.majorFunctions = {0x1000, 0, 0x3000};

    mpfshost::KDriverListData data;
    data.drivers.push_back(driver);

    const auto encoded = mpfshost::encodeKDriverListData(data);
    REQUIRE(mpfshost::kernelDriverEncodedSize(driver) == encoded.size() - sizeof(std::uint32_t));

    driver.majorFunctions.clear();
    REQUIRE(mpfshost::kernelDriverEncodedSize(driver)
            == 48 + driver.name.size() + driver.path.size() + driver.serviceKeyName.size());
}

TEST_CASE("kernel driver batch budget yields multiple frames for a large result", "[core][kerneldrivers][batching]")
{
    std::vector<mpfshost::KernelDriverDescriptor> drivers(4000);
    for (auto& driver : drivers)
    {
        driver.name           = std::string(64, 'a');
        driver.path           = std::string(64, 'b');
        driver.serviceKeyName = std::string(64, 'c');
        driver.majorFunctions = {0x1000, 0x2000};
    }

    std::size_t batches = 0;
    for (std::size_t offset = 0; offset < drivers.size();)
    {
        const auto count =
            mpfshost::batchCount(drivers, offset, mpfshost::kMaxMapBatchBytes, mpfshost::kernelDriverEncodedSize);
        REQUIRE(count > 0);
        offset += count;
        ++batches;
    }
    REQUIRE(batches > 1);
}
