#include "core/util/display.hpp"

namespace mpfshost
{
    std::string exportNamesArrayText(std::uint32_t offset)
    {
        if (offset == kAbsentArrayOffset)
        {
            return "-";
        }
        return std::to_string(offset);
    }

    std::string kernelDeviceTypeText(std::string_view name)
    {
        for (const char character : name)
        {
            if (character != '-')
            {
                return std::string(name);
            }
        }
        return "-";
    }
} // namespace mpfshost
