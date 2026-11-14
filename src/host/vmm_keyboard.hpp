#ifndef MPFSHOST_HOST_VMM_KEYBOARD_HPP
#define MPFSHOST_HOST_VMM_KEYBOARD_HPP

#pragma once

#include "core/server/keyboard_provider.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "vmmdll.h"

namespace mpfshost
{
    // Resolves and caches the guest's async key-state address. The resolution scans several megabytes of kernel
    // driver images, so it is kept per session and revalidated only when a state read fails.
    class VmmKeyboardResolver
    {
    public:
        [[nodiscard]] std::optional<KeyboardStateResponse>
        read(VMM_HANDLE handle, const KeyboardStateRequest& request, std::string& error, bool& unavailable);

    private:
        struct Resolution
        {
            std::uint32_t sessionId {};
            std::uint32_t proxyPid {};
            std::uint32_t windowsBuild {};
            std::uint64_t keyStateAddress {};
            std::uint64_t moduleBase {};
            std::uint64_t keyStateOffset {};
            std::string   moduleName;
        };

        [[nodiscard]] std::optional<Resolution>
        resolve(VMM_HANDLE handle, std::uint32_t requestedSession, std::string& error, bool& unavailable) const;

        std::mutex                                    _mutex;
        std::unordered_map<std::uint32_t, Resolution> _cache;
    };
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_KEYBOARD_HPP
