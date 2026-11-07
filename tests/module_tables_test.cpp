#include <catch2/catch.hpp>

#include "core/server/map_batching.hpp"
#include "core/server/module_table_provider.hpp"

#include <cstddef>
#include <string>
#include <vector>

TEST_CASE("export batch budget yields multiple frames for a large table", "[core][batching]")
{
    std::vector<mpfshost::ExportDescriptor> exports(20000);
    for (auto& entry : exports)
    {
        entry.function          = std::string(48, 'a');
        entry.forwardedFunction = std::string(32, 'b');
    }

    std::size_t batches = 0;
    for (std::size_t offset = 0; offset < exports.size();)
    {
        const auto count =
            mpfshost::batchCount(exports, offset, mpfshost::kMaxMapBatchBytes, mpfshost::exportEncodedSize);
        REQUIRE(count > 0);
        offset += count;
        ++batches;
    }
    REQUIRE(batches > 1);
}

TEST_CASE("import batch budget yields multiple frames for a large table", "[core][batching]")
{
    std::vector<mpfshost::ImportDescriptor> imports(20000);
    for (auto& entry : imports)
    {
        entry.function = std::string(40, 'a');
        entry.module   = std::string(20, 'b');
    }

    std::size_t batches = 0;
    for (std::size_t offset = 0; offset < imports.size();)
    {
        const auto count =
            mpfshost::batchCount(imports, offset, mpfshost::kMaxMapBatchBytes, mpfshost::importEncodedSize);
        REQUIRE(count > 0);
        offset += count;
        ++batches;
    }
    REQUIRE(batches > 1);
}

TEST_CASE("module table encoded sizes match the wire layout", "[core][batching]")
{
    mpfshost::ExportDescriptor exportEntry;
    exportEntry.function          = "abcdef";
    exportEntry.forwardedFunction = "gh";
    REQUIRE(mpfshost::exportEncodedSize(exportEntry)
            == 28 + exportEntry.function.size() + exportEntry.forwardedFunction.size());

    mpfshost::ImportDescriptor importEntry;
    importEntry.function = "abc";
    importEntry.module   = "de";
    REQUIRE(mpfshost::importEncodedSize(importEntry) == 27 + importEntry.function.size() + importEntry.module.size());
}

TEST_CASE("module table batch budget still advances for an oversized entry", "[core][batching]")
{
    std::vector<mpfshost::ExportDescriptor> exports(1);
    exports[0].function = std::string(600000, 'a');

    REQUIRE(mpfshost::exportEncodedSize(exports[0]) > mpfshost::kMaxMapBatchBytes);
    REQUIRE(mpfshost::batchCount(exports, 0, mpfshost::kMaxMapBatchBytes, mpfshost::exportEncodedSize) == 1);
}
