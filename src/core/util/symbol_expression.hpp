#ifndef MPFSHOST_CORE_UTIL_SYMBOL_EXPRESSION_HPP
#define MPFSHOST_CORE_UTIL_SYMBOL_EXPRESSION_HPP

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mpfshost
{
    // A process address expression: `module!symbol+offset`, `module!+offset`, `module+offset` or a bare
    // module name. A raw hex address is parsed by the caller before it reaches this type because the two
    // grammars overlap for names made only of hex digits.
    struct SymbolAddress
    {
        std::string   module;
        std::string   symbol; // empty for a module-relative offset or a module base
        std::uint64_t offset {};

        [[nodiscard]] bool hasModule() const noexcept
        {
            return !module.empty();
        }
    };

    // Returns nullopt when `text` is not a well-formed expression. `error` is only filled for a malformed
    // expression, never for a symbol that the host later fails to find.
    [[nodiscard]] std::optional<SymbolAddress> parseSymbolAddress(std::string_view text, std::string& error);

    // Renders the canonical `module!symbol+offset` form used in client output.
    [[nodiscard]] std::string formatSymbolAddress(const SymbolAddress& address);
} // namespace mpfshost

#endif // MPFSHOST_CORE_UTIL_SYMBOL_EXPRESSION_HPP
