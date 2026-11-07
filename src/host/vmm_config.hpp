#ifndef MPFSHOST_HOST_VMM_CONFIG_HPP
#define MPFSHOST_HOST_VMM_CONFIG_HPP

#pragma once

#include "core/ipc/message.hpp"

#include <string>

#include "vmmdll.h"

namespace mpfshost
{
    [[nodiscard]] bool setVmmConfig(VMM_HANDLE handle, const ConfigSetRequest& request, std::string& error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_CONFIG_HPP
