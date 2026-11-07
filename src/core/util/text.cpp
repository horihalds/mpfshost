#include "core/util/text.hpp"

#include <cstddef>

namespace mpfshost
{
    namespace
    {
        char foldAscii(char value)
        {
            if (value >= 'A' && value <= 'Z')
            {
                return static_cast<char>(value - 'A' + 'a');
            }
            return value;
        }
    } // namespace

    bool containsCaseInsensitive(std::string_view haystack, std::string_view needle)
    {
        if (needle.empty())
        {
            return true;
        }
        if (haystack.size() < needle.size())
        {
            return false;
        }
        for (std::size_t start = 0; start + needle.size() <= haystack.size(); ++start)
        {
            bool matches = true;
            for (std::size_t offset = 0; offset < needle.size(); ++offset)
            {
                if (foldAscii(haystack[start + offset]) != foldAscii(needle[offset]))
                {
                    matches = false;
                    break;
                }
            }
            if (matches)
            {
                return true;
            }
        }
        return false;
    }
} // namespace mpfshost
