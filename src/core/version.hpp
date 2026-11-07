#ifndef MPFSHOST_VERSION_HPP
#define MPFSHOST_VERSION_HPP
#pragma once

#include <string_view>

namespace mpfshost
{
    [[nodiscard]] std::string_view version() noexcept;
} // namespace mpfshost

#endif
