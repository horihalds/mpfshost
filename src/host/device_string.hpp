#ifndef MPFSHOST_HOST_DEVICE_STRING_HPP
#define MPFSHOST_HOST_DEVICE_STRING_HPP
#pragma once

#include "core/target/target.hpp"

#include <string>

namespace mpfshost
{
    [[nodiscard]] std::string qemuDeviceString(const VmTarget& target);
} // namespace mpfshost

#endif // MPFSHOST_HOST_DEVICE_STRING_HPP
