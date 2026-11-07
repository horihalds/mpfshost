#include <catch2/catch.hpp>

#include "core/server/map_batching.hpp"
#include "core/server/service_provider.hpp"

#include <cstddef>
#include <string>
#include <vector>

TEST_CASE("service filter accepts everything when the request has no filter", "[core][services]")
{
    mpfshost::ServiceDescriptor service;
    service.name = "Spooler";

    REQUIRE(mpfshost::matchesServiceFilter({""}, service));
}

TEST_CASE("service filter matches every carried text field case-insensitively", "[core][services]")
{
    mpfshost::ServiceDescriptor service;
    service.name        = "DemoSvc";
    service.displayName = "Demo Service";
    service.imagePath   = "C:\\Windows\\demo.sys";
    service.path        = "\\Registry\\Machine\\System\\DemoSvc";
    service.userType    = "Driver";
    service.account     = "LocalSystem";

    REQUIRE(mpfshost::matchesServiceFilter({"demosvc"}, service));
    REQUIRE(mpfshost::matchesServiceFilter({"demo service"}, service));
    REQUIRE(mpfshost::matchesServiceFilter({"DEMO.SYS"}, service));
    REQUIRE(mpfshost::matchesServiceFilter({"machine\\system"}, service));
    REQUIRE(mpfshost::matchesServiceFilter({"driver"}, service));
    REQUIRE(mpfshost::matchesServiceFilter({"localsystem"}, service));
    REQUIRE_FALSE(mpfshost::matchesServiceFilter({"absent"}, service));
}

TEST_CASE("service batch budget yields multiple frames for a large result", "[core][services][batching]")
{
    std::vector<mpfshost::ServiceDescriptor> services(4000);
    for (auto& service : services)
    {
        service.name    = std::string(64, 'a');
        service.account = std::string(64, 'b');
    }

    std::size_t batches = 0;
    for (std::size_t offset = 0; offset < services.size();)
    {
        const auto count =
            mpfshost::batchCount(services, offset, mpfshost::kMaxMapBatchBytes, mpfshost::serviceEncodedSize);
        REQUIRE(count > 0);
        offset += count;
        ++batches;
    }
    REQUIRE(batches > 1);
}

TEST_CASE("service encoded size matches the wire layout", "[core][services][batching]")
{
    mpfshost::ServiceDescriptor service;
    service.name        = "abc";
    service.displayName = "de";
    service.imagePath   = "fgh";
    service.path        = "i";
    service.userType    = "jk";
    service.account     = "lmn";

    const auto strings = service.name.size() + service.displayName.size() + service.imagePath.size()
                       + service.path.size() + service.userType.size() + service.account.size();
    REQUIRE(mpfshost::serviceEncodedSize(service) == 72 + strings);
}
