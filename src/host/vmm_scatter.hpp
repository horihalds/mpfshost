#ifndef MPFSHOST_HOST_VMM_SCATTER_HPP
#define MPFSHOST_HOST_VMM_SCATTER_HPP

#pragma once

#include "core/server/scatter_provider.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "vmmdll.h"

namespace mpfshost
{
    class VmmScatter : public ScatterHandle
    {
    public:
        VmmScatter(VMM_HANDLE handle, std::uint32_t pid);
        ~VmmScatter() override;

        VmmScatter(const VmmScatter&)            = delete;
        VmmScatter& operator=(const VmmScatter&) = delete;

        [[nodiscard]] bool valid() const noexcept;

        [[nodiscard]] bool prepare(const std::vector<ScatterRequestEntry>& entries, std::string& error) override;
        [[nodiscard]] bool execute(std::vector<ScatterResult>& results, std::string& error) override;
        [[nodiscard]] bool executeRead(std::vector<ScatterResult>& results, std::string& error) override;
        [[nodiscard]] bool clear(std::string& error) override;

    private:
        // A deque keeps every queued entry at a stable address: MemProcFS holds the buffer pointers until
        // VMMDLL_Scatter_CloseHandle, so relocating them would corrupt later reads.
        struct QueuedEntry
        {
            ScatterRequestEntry    entry;
            std::vector<std::byte> buffer;
            std::uint32_t          bytesRead {};
        };

        VMMDLL_SCATTER_HANDLE   _handle {nullptr};
        std::uint32_t           _pid {};
        std::uint32_t           _flags {};
        std::deque<QueuedEntry> _queued;
        bool                    _broken {};
    };
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_SCATTER_HPP
