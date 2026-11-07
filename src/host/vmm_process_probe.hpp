#ifndef MPFSHOST_HOST_VMM_PROCESS_PROBE_HPP
#define MPFSHOST_HOST_VMM_PROCESS_PROBE_HPP
#pragma once

#include <cstdint>

#include "vmmdll.h"

namespace mpfshost
{
    // A failing per-process map call is either a vanished pid or a genuine failure; probing the process separates
    // the two so the client sees NotFound rather than Internal.
    inline bool processPresent(VMM_HANDLE handle, std::uint32_t pid)
    {
        VMMDLL_PROCESS_INFORMATION info {};
        info.magic    = VMMDLL_PROCESS_INFORMATION_MAGIC;
        info.wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;
        auto size     = sizeof(VMMDLL_PROCESS_INFORMATION);
        return VMMDLL_ProcessGetInformation(handle, pid, &info, &size);
    }
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_PROCESS_PROBE_HPP
