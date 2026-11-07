#ifndef MPFSHOST_CORE_TARGET_TARGET_HPP
#define MPFSHOST_CORE_TARGET_TARGET_HPP
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace mpfshost
{
    struct VmTarget
    {
        std::string           name;
        std::uint32_t         qemuPid {};
        std::filesystem::path qmpSocket;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_TARGET_TARGET_HPP
