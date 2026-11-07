#include "core/util/number.hpp"

#include "core/util/hex.hpp"

#include <charconv>
#include <limits>

namespace mpfshost
{
    std::optional<std::uint32_t> parseDecimalU32(std::string_view text)
    {
        if (text.empty())
        {
            return std::nullopt;
        }
        std::uint32_t parsed {};
        const auto    result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec != std::errc {} || result.ptr != text.data() + text.size())
        {
            return std::nullopt;
        }
        return parsed;
    }

    std::optional<std::uint32_t> parseRangeSize(std::string_view text)
    {
        if (!text.starts_with("0x") && !text.starts_with("0X"))
        {
            return parseDecimalU32(text);
        }
        const auto parsed = mpfshost::parseHexU64(text);
        if (!parsed || *parsed > std::numeric_limits<std::uint32_t>::max())
        {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(*parsed);
    }
} // namespace mpfshost
