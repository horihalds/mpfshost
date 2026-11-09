#include "core/util/symbol_expression.hpp"

#include "core/util/hex.hpp"

#include <cstdio>

namespace mpfshost
{
    namespace
    {
        // Splits `symbol+offset` at the last `+` whose suffix parses as hex. A plus inside the symbol itself
        // stays part of the symbol when the suffix is not a number.
        bool splitOffset(std::string_view text, std::string& symbol, std::uint64_t& offset)
        {
            const auto plus = text.rfind('+');
            if (plus != std::string_view::npos)
            {
                if (const auto parsed = parseHexU64(text.substr(plus + 1)); parsed.has_value())
                {
                    symbol = std::string {text.substr(0, plus)};
                    offset = *parsed;
                    return true;
                }
            }
            symbol = std::string {text};
            offset = 0;
            return !symbol.empty();
        }

        std::string hexOffset(std::uint64_t value)
        {
            char buffer[19] {};
            std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
            return buffer;
        }
    } // namespace

    std::optional<SymbolAddress> parseSymbolAddress(std::string_view text, std::string& error)
    {
        error.clear();
        if (text.empty())
        {
            error = "an address expression cannot be empty";
            return std::nullopt;
        }

        const auto bang = text.find('!');
        if (bang != std::string_view::npos)
        {
            SymbolAddress address;
            address.module = std::string {text.substr(0, bang)};
            if (address.module.empty())
            {
                error = "an address expression needs a module name before '!'";
                return std::nullopt;
            }
            const auto rest = text.substr(bang + 1);
            if (rest.empty())
            {
                error = "an address expression needs a symbol or offset after '!'";
                return std::nullopt;
            }
            if (!splitOffset(rest, address.symbol, address.offset))
            {
                error = "an address expression needs a symbol or offset after '!'";
                return std::nullopt;
            }
            return address;
        }

        const auto plus = text.rfind('+');
        if (plus != std::string_view::npos)
        {
            SymbolAddress address;
            address.module = std::string {text.substr(0, plus)};
            if (address.module.empty())
            {
                error = "an address expression needs a module name before '+'";
                return std::nullopt;
            }
            const auto parsed = parseHexU64(text.substr(plus + 1));
            if (!parsed.has_value())
            {
                error = "an address expression needs a hex offset after '+'";
                return std::nullopt;
            }
            address.offset = *parsed;
            return address;
        }

        return SymbolAddress {std::string {text}, {}, 0};
    }

    std::string formatSymbolAddress(const SymbolAddress& address)
    {
        if (!address.hasModule())
        {
            return hexOffset(address.offset);
        }
        std::string result = address.module;
        if (!address.symbol.empty())
        {
            result += '!';
            result += address.symbol;
            result += '+';
            result += hexOffset(address.offset);
            return result;
        }
        result += '+';
        result += hexOffset(address.offset);
        return result;
    }
} // namespace mpfshost
