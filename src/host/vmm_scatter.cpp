#include "host/vmm_scatter.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace mpfshost
{
    namespace
    {
        constexpr std::uint32_t kScatterFlags = VMMDLL_FLAG_NOCACHE | VMMDLL_FLAG_NOCACHEPUT;
    }

    VmmScatter::VmmScatter(VMM_HANDLE handle, std::uint32_t pid) : _pid {pid}, _flags {kScatterFlags}
    {
        _handle = VMMDLL_Scatter_Initialize(handle, pid, _flags);
    }

    VmmScatter::~VmmScatter()
    {
        if (_handle != nullptr)
        {
            VMMDLL_Scatter_CloseHandle(_handle);
            _handle = nullptr;
        }
    }

    bool VmmScatter::valid() const noexcept
    {
        return _handle != nullptr;
    }

    bool VmmScatter::prepare(const std::vector<ScatterRequestEntry>& entries, std::string& error)
    {
        if (_handle == nullptr)
        {
            error = "scatter handle is not initialized";
            return false;
        }
        if (_broken)
        {
            error = "scatter handle is unusable after a failed prepare";
            return false;
        }

        for (const auto& entry : entries)
        {
            _queued.push_back(QueuedEntry {entry, std::vector<std::byte>(entry.size), 0});
            auto& queued = _queued.back();

            if (entry.write)
            {
                if (entry.data.size() != entry.size)
                {
                    _broken = true;
                    error   = "scatter write entry size does not match its data";
                    return false;
                }
                std::copy(entry.data.begin(), entry.data.end(), queued.buffer.begin());
                if (!VMMDLL_Scatter_PrepareWriteEx(_handle,
                                                   entry.address,
                                                   reinterpret_cast<PBYTE>(queued.buffer.data()),
                                                   static_cast<DWORD>(entry.size)))
                {
                    _broken = true;
                    error   = "VMMDLL_Scatter_PrepareWriteEx failed at " + std::to_string(entry.address);
                    return false;
                }
            }
            else if (!VMMDLL_Scatter_PrepareEx(_handle,
                                               entry.address,
                                               static_cast<DWORD>(entry.size),
                                               reinterpret_cast<PBYTE>(queued.buffer.data()),
                                               &queued.bytesRead))
            {
                _broken = true;
                error   = "VMMDLL_Scatter_PrepareEx failed at " + std::to_string(entry.address);
                return false;
            }
        }
        return true;
    }

    bool VmmScatter::execute(std::vector<ScatterResult>& results, std::string& error)
    {
        if (_handle == nullptr)
        {
            error = "scatter handle is not initialized";
            return false;
        }
        if (_broken)
        {
            error = "scatter handle is unusable after a failed prepare";
            return false;
        }

        if (!VMMDLL_Scatter_Execute(_handle))
        {
            error = "VMMDLL_Scatter_Execute failed";
            return false;
        }

        results.reserve(_queued.size());
        for (const auto& queued : _queued)
        {
            ScatterResult result;
            result.address       = queued.entry.address;
            result.requestedSize = queued.entry.size;

            if (queued.entry.write)
            {
                result.ok = true;
                results.push_back(std::move(result));
                continue;
            }

            result.bytesRead = std::min(queued.bytesRead, queued.entry.size);
            result.ok        = result.bytesRead == queued.entry.size;
            result.data.assign(queued.buffer.begin(),
                               queued.buffer.begin() + static_cast<std::ptrdiff_t>(result.bytesRead));
            results.push_back(std::move(result));
        }
        return true;
    }

    bool VmmScatter::executeRead(std::vector<ScatterResult>& results, std::string& error)
    {
        if (_handle == nullptr)
        {
            error = "scatter handle is not initialized";
            return false;
        }
        if (_broken)
        {
            error = "scatter handle is unusable after a failed prepare";
            return false;
        }

        if (!VMMDLL_Scatter_ExecuteRead(_handle))
        {
            error = "VMMDLL_Scatter_ExecuteRead failed";
            return false;
        }

        results.reserve(_queued.size());
        for (const auto& queued : _queued)
        {
            if (queued.entry.write)
            {
                continue;
            }

            ScatterResult result;
            result.address       = queued.entry.address;
            result.requestedSize = queued.entry.size;
            result.bytesRead     = std::min(queued.bytesRead, queued.entry.size);
            result.ok            = result.bytesRead == queued.entry.size;
            result.data.assign(queued.buffer.begin(),
                               queued.buffer.begin() + static_cast<std::ptrdiff_t>(result.bytesRead));
            results.push_back(std::move(result));
        }
        return true;
    }

    bool VmmScatter::clear(std::string& error)
    {
        if (_handle == nullptr)
        {
            error = "scatter handle is not initialized";
            return false;
        }

        if (!VMMDLL_Scatter_Clear(_handle, _pid, _flags))
        {
            error = "VMMDLL_Scatter_Clear failed";
            return false;
        }

        _queued.clear();
        _broken = false;
        return true;
    }
} // namespace mpfshost
