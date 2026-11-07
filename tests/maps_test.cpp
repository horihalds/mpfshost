#include <catch2/catch.hpp>

#include "core/ipc/frame.hpp"
#include "core/server/map_batching.hpp"
#include "core/server/module_provider.hpp"

#include <string>
#include <vector>

TEST_CASE("module filter accepts everything when the filter is empty", "[core][module]")
{
    const mpfshost::ModuleDescriptor module {
        0x401000, 0x401500, 0x1000, 0, 1, 0, 0, 0, false, "probe.exe", "", "", "", ""};

    REQUIRE(mpfshost::matchesModuleFilter({1, 0, ""}, module));
}

TEST_CASE("module filter matches a name substring case-insensitively", "[core][module]")
{
    const mpfshost::ModuleDescriptor module {
        0x401000, 0x401500, 0x1000, 0, 1, 0, 0, 0, false, "Probe.EXE", "", "", "", ""};

    REQUIRE(mpfshost::matchesModuleFilter({0, 0, "probe"}, module));
    REQUIRE(mpfshost::matchesModuleFilter({0, 0, "ROB"}, module));
    REQUIRE(mpfshost::matchesModuleFilter({0, 0, ".exe"}, module));
    REQUIRE_FALSE(mpfshost::matchesModuleFilter({0, 0, "notepad"}, module));
}

TEST_CASE("batch count keeps the encoded size within the budget", "[core][batching]")
{
    const std::vector<int> entries {10, 10, 10, 10};
    const auto             sizeOf = [](int value)
    {
        return static_cast<std::uint64_t>(value);
    };

    // 4-byte count + four 10-byte entries exceeds a 14-byte budget after the first entry.
    REQUIRE(mpfshost::batchCount(entries, 0, 14, sizeOf) == 1);
    REQUIRE(mpfshost::batchCount(entries, 0, 24, sizeOf) == 2);
    REQUIRE(mpfshost::batchCount(entries, 2, 14, sizeOf) == 1);
    REQUIRE(mpfshost::batchCount(entries, 0, 44, sizeOf) == 4);
}

TEST_CASE("batch count always includes one oversized entry", "[core][batching]")
{
    const std::vector<int> entries {1000, 10};
    const auto             sizeOf = [](int value)
    {
        return static_cast<std::uint64_t>(value);
    };

    REQUIRE(mpfshost::batchCount(entries, 0, 4, sizeOf) == 1);
    REQUIRE(mpfshost::batchCount(entries, 1, 4, sizeOf) == 1);
}

TEST_CASE("batch count is zero past the end", "[core][batching]")
{
    const std::vector<int> entries {10};
    const auto             sizeOf = [](int value)
    {
        return static_cast<std::uint64_t>(value);
    };

    REQUIRE(mpfshost::batchCount(entries, 1, 1024, sizeOf) == 0);
    REQUIRE(mpfshost::batchCount(std::vector<int> {}, 0, 1024, sizeOf) == 0);
}

TEST_CASE("module batch budget yields multiple frames for a large result", "[core][batching]")
{
    std::vector<mpfshost::ModuleDescriptor> modules;
    for (int i = 0; i < 300; ++i)
    {
        mpfshost::ModuleDescriptor module;
        module.name = std::string(4096, 'a');
        modules.push_back(std::move(module));
    }
    const auto sizeOf = [](const mpfshost::ModuleDescriptor& module) -> std::uint64_t
    {
        return 57 + module.name.size();
    };

    std::size_t batches = 0;
    for (std::size_t offset = 0; offset < modules.size();)
    {
        const auto count = mpfshost::batchCount(modules, offset, mpfshost::kMaxMapBatchBytes, sizeOf);
        REQUIRE(count > 0);
        offset += count;
        ++batches;
    }
    REQUIRE(batches > 1);
}
