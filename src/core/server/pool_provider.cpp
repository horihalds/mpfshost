#include "core/server/pool_provider.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace mpfshost
{
    namespace
    {
        // dwTag holds the four tag characters in little-endian order.
        [[nodiscard]] std::array<char, 4> tagText(std::uint32_t tag)
        {
            std::array<char, 4> text {};
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                text[i] = static_cast<char>((tag >> (8 * i)) & 0xFF);
            }
            return text;
        }

        [[nodiscard]] char toLower(char value)
        {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
        }

        [[nodiscard]] bool tagFilterMatches(const std::string& filter, std::uint32_t tag)
        {
            if (filter.size() > 4)
            {
                return false;
            }

            const auto text = tagText(tag);
            for (std::size_t i = 0; i < filter.size(); ++i)
            {
                if (toLower(text[i]) != toLower(filter[i]))
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    bool matchesPoolFilter(const PoolListRequest& request, const PoolDescriptor& entry)
    {
        if (request.minAddress != 0 && entry.va < request.minAddress)
        {
            return false;
        }
        if (request.maxAddress != 0 && entry.va >= request.maxAddress)
        {
            return false;
        }
        if (request.minBytes != 0 && entry.bytes < request.minBytes)
        {
            return false;
        }
        return tagFilterMatches(request.tagFilter, entry.tag);
    }

    bool matchesPoolTagFilter(const PoolTagListRequest& request, const PoolTagDescriptor& entry)
    {
        return tagFilterMatches(request.tagFilter, entry.tag);
    }
} // namespace mpfshost
