#include "host/vmm_keyboard.hpp"

#include "core/log/log.hpp"
#include "core/server/request_deadline.hpp"
#include "core/util/keyboard_state.hpp"
#include "core/util/signature.hpp"
#include "host/vmm_handle.hpp"
#include "host/vmm_text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mpfshost
{
    namespace
    {
        constexpr std::size_t kScanWindow      = 0x10000;
        constexpr DWORD       kKernelMemoryBit = VMMDLL_PID_PROCESS_WITH_KERNELMEMORY;

        // Windows 11 22H2 is 10.0.22621; 24H2 preview builds moved gSessionGlobalSlots out of win32ksgd.sys.
        constexpr std::uint32_t kWin11Build       = 22621;
        constexpr std::uint32_t kWin11Win32kBuild = 22632;

        constexpr std::string_view kWin32kbase = "win32kbase.sys";
        constexpr std::string_view kWin32ksgd  = "win32ksgd.sys";
        constexpr std::string_view kWin32k     = "win32k.sys";

        // `gSessionGlobalSlots` is reached via a RIP-relative load. The instruction is 7 bytes and the rel32
        // starts at offset 3.
        constexpr std::size_t kRipDispOffset = 3;
        constexpr std::size_t kRipLength     = 7;

        constexpr std::string_view kSessionGlobalSlotsSgd    = "48 8B 05 ? ? ? ? 48 8B 04 C8 48 83 C4 ? C3";
        constexpr std::string_view kSessionGlobalSlotsWin32k = "48 8B 05 ? ? ? ? FF C9 48 8B 04 C8 C3";

        // Key-state offset inside the session state structure, captured directly from `_GetAsyncKeyState`.
        constexpr std::string_view kKeyOffset24H2 = "B9 00 80 FF FF ? 22 B4 ? u4 41";
        constexpr std::string_view kKeyOffset25H2 = "40 0F B6 ? 48 C1 E9 02 8A 94 08 u4";

        struct ModuleInfo
        {
            std::uint64_t base {};
            std::uint64_t size {};
            std::string   name;
        };

        struct ProxyProcess
        {
            std::uint32_t pid {};
            std::uint32_t sessionId {};
        };

        bool equalsIgnoreCase(std::string_view left, std::string_view right)
        {
            if (left.size() != right.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < left.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(left[i]))
                    != std::tolower(static_cast<unsigned char>(right[i])))
                {
                    return false;
                }
            }
            return true;
        }

        bool tryReadMode(
            VMM_HANDLE handle, std::uint32_t pid, std::uint64_t address, std::span<std::byte> out, bool kernelMemory)
        {
            if (handle == nullptr || address == 0 || out.empty())
            {
                return false;
            }
            DWORD      bytesRead {};
            const auto flags   = VMMDLL_FLAG_NOCACHE | VMMDLL_FLAG_NOCACHEPUT;
            const auto readPid = kernelMemory ? (pid | kKernelMemoryBit) : pid;
            return VMMDLL_MemReadEx(handle,
                                    readPid,
                                    address,
                                    reinterpret_cast<PBYTE>(out.data()),
                                    static_cast<DWORD>(out.size()),
                                    &bytesRead,
                                    flags)
                && bytesRead == out.size();
        }

        // Tries the last known-good read mode first. A successful fallback updates the mode for later calls.
        bool tryReadPreferred(
            VMM_HANDLE handle, std::uint32_t pid, std::uint64_t address, std::span<std::byte> out, bool& kernelMemory)
        {
            if (tryReadMode(handle, pid, address, out, kernelMemory))
            {
                return true;
            }
            const auto other = !kernelMemory;
            if (tryReadMode(handle, pid, address, out, other))
            {
                kernelMemory = other;
                return true;
            }
            return false;
        }

        bool tryRead(VMM_HANDLE handle, std::uint32_t pid, std::uint64_t address, std::span<std::byte> out)
        {
            bool kernelMemory = false;
            return tryReadPreferred(handle, pid, address, out, kernelMemory);
        }

        std::optional<std::uint64_t>
        readPointer(VMM_HANDLE handle, std::uint32_t pid, std::uint64_t address, bool& kernelMemory)
        {
            std::array<std::byte, sizeof(std::uint64_t)> buffer {};
            if (!tryReadPreferred(handle, pid, address, buffer, kernelMemory))
            {
                return std::nullopt;
            }
            std::uint64_t value = 0;
            for (std::size_t i = 0; i < buffer.size(); ++i)
            {
                value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(buffer[i])) << (i * 8);
            }
            return value;
        }

        std::uint32_t readU32At(std::span<const std::byte> data, std::size_t offset)
        {
            std::uint32_t value = 0;
            for (std::size_t i = 0; i < 4; ++i)
            {
                value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[offset + i])) << (i * 8);
            }
            return value;
        }

        std::optional<ModuleInfo> findDriverModule(VMM_HANDLE handle, std::string_view name)
        {
            // Kernel drivers are not driver objects in the KDriver map; their images are listed as modules of the
            // system process, which is pid 4 on every supported Windows.
            constexpr DWORD kSystemPid = 4;

            PVMMDLL_MAP_MODULE map = nullptr;
            if (!VMMDLL_Map_GetModuleU(handle, kSystemPid, &map, 0) || map == nullptr)
            {
                return std::nullopt;
            }
            const std::unique_ptr<VMMDLL_MAP_MODULE, decltype(&VMMDLL_MemFree)> guard {map, &VMMDLL_MemFree};

            for (DWORD i = 0; i < map->cMap; ++i)
            {
                const auto& entry = map->pMap[i];
                if (!equalsIgnoreCase(copyMapText(entry.uszText), name))
                {
                    continue;
                }
                ModuleInfo module;
                module.base = entry.vaBase;
                module.size = entry.cbImageSize;
                module.name = std::string {name};
                return module;
            }
            return std::nullopt;
        }

        std::optional<ProxyProcess> findProxyProcess(VMM_HANDLE handle, std::uint32_t requestedSession)
        {
            static constexpr std::string_view kCandidates[] = {
                "winlogon.exe",
                "explorer.exe",
                "dwm.exe",
                "taskhostw.exe",
                "smartscreen.exe",
            };

            SIZE_T count = 0;
            if (!VMMDLL_PidList(handle, nullptr, &count) || count == 0)
            {
                return std::nullopt;
            }
            std::vector<DWORD> pids(count);
            if (!VMMDLL_PidList(handle, pids.data(), &count))
            {
                return std::nullopt;
            }
            pids.resize(count);

            std::optional<ProxyProcess> fallback;
            for (const auto candidate : kCandidates)
            {
                for (const DWORD pid : pids)
                {
                    VMMDLL_PROCESS_INFORMATION info {};
                    info.magic    = VMMDLL_PROCESS_INFORMATION_MAGIC;
                    info.wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;
                    auto size     = sizeof(VMMDLL_PROCESS_INFORMATION);
                    if (!VMMDLL_ProcessGetInformation(handle, pid, &info, &size))
                    {
                        continue;
                    }

                    std::string_view name = info.szNameLong[0] != '\0' ? std::string_view {info.szNameLong}
                                                                       : std::string_view {info.szName};
                    if (!equalsIgnoreCase(name, candidate))
                    {
                        continue;
                    }

                    ProxyProcess proxy;
                    proxy.pid       = info.dwPID;
                    proxy.sessionId = info.win.dwSessionId;
                    if (requestedSession == 0 || proxy.sessionId == requestedSession)
                    {
                        return proxy;
                    }
                    if (!fallback)
                    {
                        fallback = proxy;
                    }
                }
            }
            return fallback;
        }

        std::optional<SignatureMatch>
        scanModule(VMM_HANDLE handle, std::uint32_t pid, const ModuleInfo& module, const SignaturePattern& pattern)
        {
            if (module.size == 0)
            {
                return std::nullopt;
            }

            const std::size_t      overlap = pattern.bytes.size() > 1 ? pattern.bytes.size() - 1 : 0;
            std::vector<std::byte> buffer;
            for (std::uint64_t offset = 0; offset < module.size; offset += kScanWindow)
            {
                if (requestExpired())
                {
                    return std::nullopt;
                }

                const auto remaining = module.size - offset;
                const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(kScanWindow + overlap, remaining));
                buffer.resize(wanted);
                if (!tryRead(handle, pid, module.base + offset, buffer))
                {
                    continue;
                }
                if (auto match = findSignature(buffer, pattern))
                {
                    match->offset += static_cast<std::size_t>(offset);
                    return match;
                }
            }
            return std::nullopt;
        }

        std::optional<std::uint64_t>
        scanRipRelative(VMM_HANDLE handle, std::uint32_t pid, const ModuleInfo& module, const SignaturePattern& pattern)
        {
            const auto match = scanModule(handle, pid, module, pattern);
            if (!match)
            {
                return std::nullopt;
            }

            std::array<std::byte, 4> displacement {};
            if (!tryRead(handle, pid, module.base + match->offset + kRipDispOffset, displacement))
            {
                return std::nullopt;
            }
            const auto raw    = readU32At(displacement, 0);
            const auto disp   = static_cast<std::int32_t>(raw);
            const auto target = static_cast<std::int64_t>(match->offset + kRipLength) + disp;
            if (target < 0)
            {
                return std::nullopt;
            }
            return static_cast<std::uint64_t>(target);
        }

        std::optional<std::uint32_t>
        scanCapture(VMM_HANDLE handle, std::uint32_t pid, const ModuleInfo& module, const SignaturePattern& pattern)
        {
            const auto match = scanModule(handle, pid, module, pattern);
            if (!match)
            {
                return std::nullopt;
            }
            return match->captured;
        }

        struct SessionGlobalSlots
        {
            std::uint64_t address {};
            ModuleInfo    module;
        };

        std::optional<SessionGlobalSlots>
        resolveSessionGlobalSlots(VMM_HANDLE handle, std::uint32_t pid, std::uint32_t build, std::string& error)
        {
            const auto primaryName = build >= kWin11Win32kBuild ? kWin32k : kWin32ksgd;
            const auto otherName   = build >= kWin11Win32kBuild ? kWin32ksgd : kWin32k;

            const std::array<std::pair<std::string_view, std::string_view>, 2> choices {
                std::pair {primaryName,
                           build >= kWin11Win32kBuild ? kSessionGlobalSlotsWin32k : kSessionGlobalSlotsSgd             },
                std::pair {  otherName, build >= kWin11Win32kBuild ? kSessionGlobalSlotsSgd : kSessionGlobalSlotsWin32k},
            };

            for (const auto& [moduleName, signatureText] : choices)
            {
                const auto module = findDriverModule(handle, moduleName);
                if (!module)
                {
                    continue;
                }
                const auto pattern = parseSignature(signatureText);
                if (!pattern)
                {
                    continue;
                }
                if (const auto rva = scanRipRelative(handle, pid, *module, *pattern))
                {
                    SessionGlobalSlots result;
                    result.address = module->base + *rva;
                    result.module  = *module;
                    return result;
                }
            }

            error = "unable to find the Win32k session global slots";
            return std::nullopt;
        }

        std::optional<std::uint32_t>
        resolveKeyStateOffset(VMM_HANDLE handle, std::uint32_t pid, const ModuleInfo& win32kbase, std::string& error)
        {
            for (const auto text : {kKeyOffset24H2, kKeyOffset25H2})
            {
                const auto pattern = parseSignature(text);
                if (!pattern)
                {
                    continue;
                }
                if (const auto offset = scanCapture(handle, pid, win32kbase, *pattern))
                {
                    return offset;
                }
            }

            error = "unable to find the Win32k key-state offset";
            return std::nullopt;
        }
    } // namespace

    std::optional<KeyboardStateResponse> VmmKeyboardResolver::read(VMM_HANDLE                  handle,
                                                                   const KeyboardStateRequest& request,
                                                                   std::string&                error,
                                                                   bool&                       unavailable)
    {
        if (handle == nullptr)
        {
            error       = "no MemProcFS handle is available";
            unavailable = true;
            return std::nullopt;
        }

        std::lock_guard lock {_mutex};

        auto remember = [&](const Resolution& resolution)
        {
            _cache[request.sessionId] = resolution;
            if (resolution.sessionId != request.sessionId)
            {
                _cache[resolution.sessionId] = resolution;
            }
        };
        auto forget = [&](const Resolution& resolution)
        {
            _cache.erase(request.sessionId);
            if (resolution.sessionId != request.sessionId)
            {
                _cache.erase(resolution.sessionId);
            }
        };

        auto cached = _cache.find(request.sessionId);
        if (cached != _cache.end())
        {
            auto                  resolution = cached->second;
            KeyboardStateResponse response;
            log::debug("keyboard", "cache hit for session ", resolution.sessionId);
            if (validate(handle, resolution) && readState(handle, resolution, response))
            {
                remember(resolution);
                return response;
            }

            log::info("keyboard", "cached keyboard state for session ", resolution.sessionId, " is stale");
            forget(resolution);
        }

        log::debug("keyboard", "resolving the keyboard state");
        auto resolution = resolve(handle, request.sessionId, error, unavailable);
        if (!resolution)
        {
            return std::nullopt;
        }

        KeyboardStateResponse response;
        if (!readState(handle, *resolution, response))
        {
            error = "the resolved keyboard state is not readable";
            return std::nullopt;
        }

        remember(*resolution);
        log::debug("keyboard", "resolved the keyboard state for session ", resolution->sessionId);
        return response;
    }

    std::optional<VmmKeyboardResolver::Resolution> VmmKeyboardResolver::resolve(VMM_HANDLE    handle,
                                                                                std::uint32_t requestedSession,
                                                                                std::string&  error,
                                                                                bool&         unavailable) const
    {
        ULONG64 system = 0;
        if (VMMDLL_ConfigGet(handle, VMMDLL_OPT_CORE_SYSTEM, &system) && system != VMMDLL_SYSTEM_WINDOWS_64
            && system != VMMDLL_SYSTEM_WINDOWS_32)
        {
            error       = "the target is not a Windows system";
            unavailable = true;
            return std::nullopt;
        }

        ULONG64 buildValue = 0;
        if (!VMMDLL_ConfigGet(handle, VMMDLL_OPT_WIN_VERSION_BUILD, &buildValue))
        {
            error       = "the target's Windows build is unknown";
            unavailable = true;
            return std::nullopt;
        }
        if (buildValue < kWin11Build)
        {
            error       = "the keyboard state needs Windows 11 22H2 or newer";
            unavailable = true;
            return std::nullopt;
        }
        const auto build = static_cast<std::uint32_t>(buildValue);

        const auto proxy = findProxyProcess(handle, requestedSession);
        if (!proxy)
        {
            error       = "unable to find a proxy process for the keyboard state";
            unavailable = true;
            return std::nullopt;
        }

        const auto win32kbase = findDriverModule(handle, kWin32kbase);
        if (!win32kbase)
        {
            error = "unable to find win32kbase.sys";
            return std::nullopt;
        }

        const auto globalSlots = resolveSessionGlobalSlots(handle, proxy->pid, build, error);
        if (!globalSlots)
        {
            return std::nullopt;
        }

        const auto keyStateOffset = resolveKeyStateOffset(handle, proxy->pid, *win32kbase, error);
        if (!keyStateOffset)
        {
            return std::nullopt;
        }

        const auto sessionId =
            requestedSession != 0 ? requestedSession : (proxy->sessionId != 0 ? proxy->sessionId : 1);
        if (sessionId == 0 || sessionId > kMaxKeyboardSessions)
        {
            error = "the requested keyboard session does not exist";
            return std::nullopt;
        }

        bool kernelMemory = false;

        const auto slotArray = readPointer(handle, proxy->pid, globalSlots->address, kernelMemory);
        if (!slotArray || *slotArray == 0)
        {
            error = "the Win32k session global slots are not readable";
            return std::nullopt;
        }

        const auto slotAddress = *slotArray + static_cast<std::uint64_t>(sessionId - 1) * 8;
        const auto slot        = readPointer(handle, proxy->pid, slotAddress, kernelMemory);
        if (!slot || *slot == 0)
        {
            error = "the requested keyboard session state is not readable";
            return std::nullopt;
        }

        const auto sessionState = readPointer(handle, proxy->pid, *slot, kernelMemory);
        if (!sessionState || *sessionState == 0)
        {
            error = "the requested keyboard session state is not readable";
            return std::nullopt;
        }

        Resolution resolution;
        resolution.sessionId       = sessionId;
        resolution.proxyPid        = proxy->pid;
        resolution.windowsBuild    = build;
        resolution.triggerAddress  = globalSlots->address;
        resolution.statePointer    = *sessionState;
        resolution.keyStateAddress = *sessionState + *keyStateOffset;
        resolution.moduleBase      = globalSlots->module.base;
        resolution.keyStateOffset  = *keyStateOffset;
        resolution.moduleName      = globalSlots->module.name;
        resolution.kernelMemory    = kernelMemory;
        return resolution;
    }

    bool VmmKeyboardResolver::validate(VMM_HANDLE handle, Resolution& resolution) const
    {
        const auto slotArray =
            readPointer(handle, resolution.proxyPid, resolution.triggerAddress, resolution.kernelMemory);
        if (!slotArray || *slotArray == 0)
        {
            return false;
        }

        const auto slotAddress = *slotArray + static_cast<std::uint64_t>(resolution.sessionId - 1) * 8;
        const auto slot        = readPointer(handle, resolution.proxyPid, slotAddress, resolution.kernelMemory);
        if (!slot || *slot == 0)
        {
            return false;
        }

        const auto state = readPointer(handle, resolution.proxyPid, *slot, resolution.kernelMemory);
        return state && *state == resolution.statePointer;
    }

    bool
    VmmKeyboardResolver::readState(VMM_HANDLE handle, Resolution& resolution, KeyboardStateResponse& response) const
    {
        response.sessionId       = resolution.sessionId;
        response.proxyPid        = resolution.proxyPid;
        response.windowsBuild    = resolution.windowsBuild;
        response.keyStateAddress = resolution.keyStateAddress;
        response.moduleBase      = resolution.moduleBase;
        response.keyStateOffset  = resolution.keyStateOffset;
        response.moduleName      = resolution.moduleName;
        response.state.resize(kKeyboardStateBytes);

        if (!tryReadPreferred(handle,
                              resolution.proxyPid,
                              resolution.keyStateAddress,
                              std::span<std::byte> {response.state.data(), response.state.size()},
                              resolution.kernelMemory))
        {
            return false;
        }

        response.pressedKeys = decodeKeyboardState(response.state);
        return true;
    }

    std::optional<KeyboardStateResponse>
    VmmHandle::keyboardState(const KeyboardStateRequest& request, std::string& error, bool& unavailable)
    {
        return _keyboard.read(_handle, request, error, unavailable);
    }
} // namespace mpfshost
