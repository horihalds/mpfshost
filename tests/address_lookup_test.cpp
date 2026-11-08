#include <catch2/catch.hpp>

#include "core/server/module_lookup_provider.hpp"
#include "core/server/process_info_provider.hpp"

#include <cstdint>
#include <limits>
#include <vector>

namespace
{
    mpfshost::ModuleDescriptor moduleAt(std::uint64_t base, std::uint32_t size, std::string name)
    {
        mpfshost::ModuleDescriptor module;
        module.base      = base;
        module.imageSize = size;
        module.name      = std::move(name);
        return module;
    }
} // namespace

TEST_CASE("module lookup uses a half-open image range", "[core][module]")
{
    const std::vector<mpfshost::ModuleDescriptor> modules = {
        moduleAt(0x10000, 0x2000, "alpha.exe"),
        moduleAt(0x100000, 0x4000, "beta.dll"),
    };

    REQUIRE(mpfshost::findModuleAtAddress(modules, 0x10000) == &modules[0]);
    REQUIRE(mpfshost::findModuleAtAddress(modules, 0x11fff) == &modules[0]);
    REQUIRE(mpfshost::findModuleAtAddress(modules, 0x12000) == nullptr);
    REQUIRE(mpfshost::findModuleAtAddress(modules, 0x100000) == &modules[1]);
    REQUIRE(mpfshost::findModuleAtAddress(modules, 0x103fff) == &modules[1]);
    REQUIRE(mpfshost::findModuleAtAddress(modules, 0x104000) == nullptr);
    REQUIRE(mpfshost::findModuleAtAddress(modules, 0) == nullptr);
}

TEST_CASE("module lookup ignores empty and overflowing ranges", "[core][module]")
{
    const std::vector<mpfshost::ModuleDescriptor> modules = {
        moduleAt(0xfffffffffffff000ull, 0x1000, "high"),
        moduleAt(0x2000, 0, "empty"),
    };

    REQUIRE(mpfshost::findModuleAtAddress(modules, 0xffffffffffffffffull) == &modules[0]);
    REQUIRE(mpfshost::findModuleAtAddress(modules, 0x2000) == nullptr);
    REQUIRE(mpfshost::findModuleAtAddress({}, std::numeric_limits<std::uint64_t>::max()) == nullptr);
}

TEST_CASE("dtb comparison ignores the low cr3 flag bits", "[core][process]")
{
    REQUIRE(mpfshost::canonicalDtb(0x1aa000) == 0x1aa000);
    REQUIRE(mpfshost::canonicalDtb(0x1aafff) == 0x1aa000);
    REQUIRE(mpfshost::canonicalDtb(0x1aa001) == 0x1aa000);
    REQUIRE(mpfshost::canonicalDtb(0xffffffffffffffffull) == 0xfffffffffffff000ull);
    REQUIRE(mpfshost::canonicalDtb(0xfff) == 0);
}
