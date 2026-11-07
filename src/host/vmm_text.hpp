#ifndef MPFSHOST_HOST_VMM_TEXT_HPP
#define MPFSHOST_HOST_VMM_TEXT_HPP
#pragma once

#include "core/ipc/frame.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#include "vmmdll.h"

namespace mpfshost
{
    // uszText points into a MemProcFS map allocation, so copy it before the map is freed and cap it so one entry
    // can never grow a frame beyond the payload limit.
    inline std::string copyMapText(LPCSTR text)
    {
        if (text == nullptr)
        {
            return {};
        }
        const std::size_t length = std::min<std::size_t>(std::strlen(text), kMaxMapStringLength);
        return std::string {text, length};
    }

    // An NTSTATUS is only usable in hexadecimal; the decimal rendering cannot be looked up anywhere.
    [[nodiscard]] inline std::string formatStatus(NTSTATUS status)
    {
        char buffer[16] {};
        std::snprintf(buffer, sizeof(buffer), "0x%08x", static_cast<unsigned>(status));
        return buffer;
    }
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_TEXT_HPP
