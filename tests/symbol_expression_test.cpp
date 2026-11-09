#include "core/util/symbol_expression.hpp"

#include <catch2/catch.hpp>

TEST_CASE("symbol expressions parse module, symbol and offset forms", "[core][util][symbol]")
{
    std::string error;

    const auto symbol = mpfshost::parseSymbolAddress("nt!HalProcessorIdle+0xf", error);
    REQUIRE(symbol.has_value());
    REQUIRE(symbol->module == "nt");
    REQUIRE(symbol->symbol == "HalProcessorIdle");
    REQUIRE(symbol->offset == 0xf);

    const auto moduleOffset = mpfshost::parseSymbolAddress("guest-probe.exe+0x1234", error);
    REQUIRE(moduleOffset.has_value());
    REQUIRE(moduleOffset->module == "guest-probe.exe");
    REQUIRE(moduleOffset->symbol.empty());
    REQUIRE(moduleOffset->offset == 0x1234);

    const auto moduleOnly = mpfshost::parseSymbolAddress("nt", error);
    REQUIRE(moduleOnly.has_value());
    REQUIRE(moduleOnly->module == "nt");
    REQUIRE(moduleOnly->symbol.empty());
    REQUIRE(moduleOnly->offset == 0);

    const auto relative = mpfshost::parseSymbolAddress("nt!+0x10", error);
    REQUIRE(relative.has_value());
    REQUIRE(relative->module == "nt");
    REQUIRE(relative->symbol.empty());
    REQUIRE(relative->offset == 0x10);

    const auto plusInSymbol = mpfshost::parseSymbolAddress("guest-probe.exe!operator++", error);
    REQUIRE(plusInSymbol.has_value());
    REQUIRE(plusInSymbol->module == "guest-probe.exe");
    REQUIRE(plusInSymbol->symbol == "operator++");
    REQUIRE(plusInSymbol->offset == 0);
}

TEST_CASE("symbol expressions reject malformed forms", "[core][util][symbol]")
{
    std::string error;
    REQUIRE_FALSE(mpfshost::parseSymbolAddress("", error).has_value());
    REQUIRE_FALSE(mpfshost::parseSymbolAddress("!KiIdleLoop", error).has_value());
    REQUIRE_FALSE(mpfshost::parseSymbolAddress("nt!", error).has_value());
    REQUIRE_FALSE(mpfshost::parseSymbolAddress("nt+nope", error).has_value());
    REQUIRE_FALSE(mpfshost::parseSymbolAddress("+0x10", error).has_value());
}

TEST_CASE("symbol expressions format back to the documented form", "[core][util][symbol]")
{
    REQUIRE(mpfshost::formatSymbolAddress({"nt", "HalProcessorIdle", 0xf}) == "nt!HalProcessorIdle+0xf");
    REQUIRE(mpfshost::formatSymbolAddress({"guest-probe.exe", {}, 0x1234}) == "guest-probe.exe+0x1234");
    REQUIRE(mpfshost::formatSymbolAddress({"", {}, 0x20}) == "0x20");
}
