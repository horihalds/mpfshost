#ifndef MPFSHOST_CORE_UTIL_TEXT_HPP
#define MPFSHOST_CORE_UTIL_TEXT_HPP

#pragma once

#include <string_view>

namespace mpfshost
{
    [[nodiscard]] bool containsCaseInsensitive(std::string_view haystack, std::string_view needle);
} // namespace mpfshost

#endif // MPFSHOST_CORE_UTIL_TEXT_HPP
