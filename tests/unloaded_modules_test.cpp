#include <catch2/catch.hpp>

#include "core/server/map_batching.hpp"
#include "core/server/unloaded_module_provider.hpp"

#include <cstddef>
#include <string>
#include <vector>

TEST_CASE("unloaded module filter accepts everything when the request has no filter", "[core][unloaded]")
{
    mpfshost::UnloadedModuleDescriptor module;
    module.text = "gone.dll";

    REQUIRE(mpfshost::matchesUnloadedModuleFilter({1000, ""}, module));
}

TEST_CASE("unloaded module filter matches the name case-insensitively", "[core][unloaded]")
{
    mpfshost::UnloadedModuleDescriptor module;
    module.text = "oldtool.exe";

    REQUIRE(mpfshost::matchesUnloadedModuleFilter({1000, "OLD"}, module));
    REQUIRE(mpfshost::matchesUnloadedModuleFilter({1000, "tool.exe"}, module));
    REQUIRE_FALSE(mpfshost::matchesUnloadedModuleFilter({1000, "newtool"}, module));
}

TEST_CASE("unloaded module batch budget yields multiple frames for a large result", "[core][batching]")
{
    std::vector<mpfshost::UnloadedModuleDescriptor> modules(5000);
    for (auto& module : modules)
    {
        module.text = std::string(96, 'a');
    }

    std::size_t batches = 0;
    for (std::size_t offset = 0; offset < modules.size();)
    {
        const auto count =
            mpfshost::batchCount(modules, offset, mpfshost::kMaxMapBatchBytes, mpfshost::unloadedModuleEncodedSize);
        REQUIRE(count > 0);
        offset += count;
        ++batches;
    }
    REQUIRE(batches > 1);
}

TEST_CASE("unloaded module encoded size matches the wire layout", "[core][batching]")
{
    mpfshost::UnloadedModuleDescriptor module;
    module.text = "abcde";
    REQUIRE(mpfshost::unloadedModuleEncodedSize(module) == 33 + module.text.size());
}
