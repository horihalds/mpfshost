#include "core/util/signature.hpp"

#include <cctype>
#include <limits>

namespace mpfshost
{
    namespace
    {
        std::optional<std::uint8_t> hexByte(std::string_view token)
        {
            if (token.size() != 2)
            {
                return std::nullopt;
            }

            std::uint8_t value = 0;
            for (const char character : token)
            {
                const auto digit = static_cast<unsigned char>(character);
                value <<= 4;
                if (digit >= '0' && digit <= '9')
                {
                    value = static_cast<std::uint8_t>(value | (digit - '0'));
                }
                else if (digit >= 'a' && digit <= 'f')
                {
                    value = static_cast<std::uint8_t>(value | (digit - 'a' + 10));
                }
                else if (digit >= 'A' && digit <= 'F')
                {
                    value = static_cast<std::uint8_t>(value | (digit - 'A' + 10));
                }
                else
                {
                    return std::nullopt;
                }
            }
            return value;
        }

        bool isSpace(char character)
        {
            return std::isspace(static_cast<unsigned char>(character)) != 0;
        }
    } // namespace

    std::optional<SignaturePattern> parseSignature(std::string_view text)
    {
        SignaturePattern pattern;
        std::size_t      index = 0;
        while (index < text.size())
        {
            while (index < text.size() && isSpace(text[index]))
            {
                ++index;
            }
            if (index >= text.size())
            {
                break;
            }

            std::size_t end = index;
            while (end < text.size() && !isSpace(text[end]))
            {
                ++end;
            }
            const std::string_view token = text.substr(index, end - index);
            index                        = end;

            if (token == "?")
            {
                pattern.bytes.push_back(std::byte {0});
                pattern.mask.push_back(std::byte {0});
                continue;
            }
            if (token == "u4")
            {
                if (pattern.hasCapture)
                {
                    return std::nullopt;
                }
                pattern.captureOffset = pattern.bytes.size();
                pattern.hasCapture    = true;
                for (int i = 0; i < 4; ++i)
                {
                    pattern.bytes.push_back(std::byte {0});
                    pattern.mask.push_back(std::byte {0});
                }
                continue;
            }

            const auto value = hexByte(token);
            if (!value)
            {
                return std::nullopt;
            }
            pattern.bytes.push_back(static_cast<std::byte>(*value));
            pattern.mask.push_back(std::byte {0xFF});
        }

        if (pattern.bytes.empty())
        {
            return std::nullopt;
        }
        return pattern;
    }

    std::optional<SignatureMatch> findSignature(std::span<const std::byte> data, const SignaturePattern& pattern)
    {
        if (pattern.bytes.empty() || data.size() < pattern.bytes.size())
        {
            return std::nullopt;
        }

        const std::size_t last = data.size() - pattern.bytes.size();
        for (std::size_t start = 0; start <= last; ++start)
        {
            bool matched = true;
            for (std::size_t i = 0; i < pattern.bytes.size(); ++i)
            {
                if ((pattern.mask[i] & data[start + i]) != (pattern.mask[i] & pattern.bytes[i]))
                {
                    matched = false;
                    break;
                }
            }
            if (!matched)
            {
                continue;
            }

            SignatureMatch match;
            match.offset = start;
            if (pattern.hasCapture)
            {
                std::uint32_t captured = 0;
                for (std::size_t i = 0; i < 4; ++i)
                {
                    captured |= static_cast<std::uint32_t>(data[start + pattern.captureOffset + i]) << (i * 8);
                }
                match.captured = captured;
            }
            return match;
        }
        return std::nullopt;
    }
} // namespace mpfshost
