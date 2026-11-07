#include "core/util/hex.hpp"

#include "core/ipc/frame.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace mpfshost
{
    namespace
    {
        constexpr int hexDigit(char value)
        {
            if (value >= '0' && value <= '9')
            {
                return value - '0';
            }
            if (value >= 'a' && value <= 'f')
            {
                return value - 'a' + 10;
            }
            if (value >= 'A' && value <= 'F')
            {
                return value - 'A' + 10;
            }
            return -1;
        }

        bool isSeparator(char value)
        {
            return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == ':' || value == ','
                || value == '-';
        }
    } // namespace

    std::optional<std::uint64_t> parseHexU64(std::string_view text)
    {
        if (text.starts_with("0x") || text.starts_with("0X"))
        {
            text.remove_prefix(2);
        }
        if (text.empty())
        {
            return std::nullopt;
        }

        std::uint64_t value = 0;
        for (const char character : text)
        {
            const int digit = hexDigit(character);
            if (digit < 0)
            {
                return std::nullopt;
            }
            if (value > (std::numeric_limits<std::uint64_t>::max() >> 4))
            {
                return std::nullopt;
            }
            value = (value << 4) | static_cast<std::uint64_t>(digit);
        }
        return value;
    }

    std::optional<std::vector<std::byte>> parseHexBytes(std::string_view text)
    {
        std::vector<std::byte> bytes;
        std::optional<int>     high;

        for (const char character : text)
        {
            if (isSeparator(character))
            {
                continue;
            }
            const int digit = hexDigit(character);
            if (digit < 0)
            {
                return std::nullopt;
            }
            if (!high.has_value())
            {
                high = digit;
                continue;
            }
            bytes.push_back(static_cast<std::byte>((*high << 4) | digit));
            high.reset();
        }

        if (high.has_value())
        {
            return std::nullopt;
        }
        return bytes;
    }

    std::string formatHexDump(std::span<const std::byte> data, std::uint64_t baseAddress)
    {
        constexpr std::size_t kBytesPerRow = 16;

        std::string result;
        for (std::size_t offset = 0; offset < data.size(); offset += kBytesPerRow)
        {
            char addressText[17];
            std::snprintf(
                addressText, sizeof(addressText), "%016llx", static_cast<unsigned long long>(baseAddress + offset));
            result += addressText;
            result += "  ";

            for (std::size_t index = 0; index < kBytesPerRow; ++index)
            {
                if (offset + index < data.size())
                {
                    char byteText[3];
                    std::snprintf(byteText, sizeof(byteText), "%02x", std::to_integer<unsigned>(data[offset + index]));
                    result += byteText;
                }
                else
                {
                    result += "  ";
                }
                result += index == 7 ? "  " : " ";
            }

            result += " |";
            for (std::size_t index = 0; index < kBytesPerRow && offset + index < data.size(); ++index)
            {
                const auto value = std::to_integer<unsigned char>(data[offset + index]);
                result += (value >= 0x20 && value <= 0x7E) ? static_cast<char>(value) : '.';
            }
            result += "|\n";
        }
        return result;
    }

    std::optional<SearchTerm> parseHexPattern(std::string_view text)
    {
        SearchTerm  term;
        std::size_t index = 0;
        while (index < text.size())
        {
            if (isSeparator(text[index]))
            {
                ++index;
                continue;
            }

            if (text[index] == '?')
            {
                std::size_t wildcards = 0;
                while (index < text.size() && text[index] == '?')
                {
                    ++index;
                    ++wildcards;
                }
                if (wildcards % 2 != 0)
                {
                    return std::nullopt;
                }
                for (std::size_t byte = 0; byte < wildcards / 2; ++byte)
                {
                    term.pattern.push_back(std::byte {0});
                    term.mask.push_back(std::byte {0xFF});
                }
            }
            else
            {
                if (index + 1 < text.size() && text[index] == '0' && (text[index + 1] == 'x' || text[index + 1] == 'X'))
                {
                    index += 2;
                }

                unsigned nibbleHigh = 0;
                bool     haveHigh   = false;
                while (index < text.size() && !isSeparator(text[index]) && text[index] != '?')
                {
                    const int digit = hexDigit(text[index]);
                    if (digit < 0)
                    {
                        return std::nullopt;
                    }
                    if (!haveHigh)
                    {
                        nibbleHigh = static_cast<unsigned>(digit);
                        haveHigh   = true;
                    }
                    else
                    {
                        term.pattern.push_back(std::byte {static_cast<unsigned char>((nibbleHigh << 4) | digit)});
                        term.mask.push_back(std::byte {0x00});
                        haveHigh = false;
                    }
                    ++index;
                }
                if (haveHigh)
                {
                    return std::nullopt;
                }
            }

            if (term.pattern.size() > kMaxSearchPatternLength)
            {
                return std::nullopt;
            }
        }

        if (term.pattern.empty())
        {
            return std::nullopt;
        }
        return term;
    }

    SearchTerm makeSearchTerm(std::string_view text)
    {
        SearchTerm term;
        term.pattern.reserve(text.size());
        term.mask.reserve(text.size());
        for (const char character : text)
        {
            term.pattern.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
            term.mask.push_back(std::byte {0x00});
        }
        return term;
    }

    bool isSearchableTerm(const SearchTerm& term) noexcept
    {
        return std::any_of(term.pattern.begin(),
                           term.pattern.end(),
                           [](std::byte value)
                           {
                               return value != std::byte {0};
                           });
    }
} // namespace mpfshost
