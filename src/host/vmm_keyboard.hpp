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
            std::uint64_t triggerAddress {};  // gSessionGlobalSlots
            std::uint64_t statePointer {};    // the session state before the key-state offset
            std::uint64_t keyStateAddress {}; // statePointer + keyStateOffset
            std::uint64_t moduleBase {};
            std::uint64_t keyStateOffset {};
            std::string   moduleName;
            bool          kernelMemory {}; // the read mode that last succeeded
        };

        [[nodiscard]] std::optional<Resolution>
        resolve(VMM_HANDLE handle, std::uint32_t requestedSession, std::string& error, bool& unavailable) const;

        // Re-walks the cached pointer chain and returns true when it still reaches the cached session state.
        [[nodiscard]] bool validate(VMM_HANDLE handle, Resolution& resolution) const;

        [[nodiscard]] bool readState(VMM_HANDLE handle, Resolution& resolution, KeyboardStateResponse& response) const;

        std::mutex                                    _mutex;
        std::unordered_map<std::uint32_t, Resolution> _cache;
    };
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_KEYBOARD_HPP
