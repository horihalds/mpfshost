#ifndef MPFSHOST_CORE_SERVER_VFS_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_VFS_PROVIDER_HPP

#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mpfshost
{
    class VfsProvider
    {
    public:
        virtual ~VfsProvider() = default;

        [[nodiscard]] virtual std::optional<std::vector<VfsEntry>>
        listVfs(const VfsListRequest& request, LookupStatus& status, std::string& error) = 0;

        [[nodiscard]] virtual bool
        readVfs(const VfsReadRequest& request, std::vector<std::byte>& out, std::string& error) = 0;

        [[nodiscard]] virtual bool
        writeVfs(const VfsWriteRequest& request, std::uint32_t& bytesWritten, std::string& error) = 0;
    };

    [[nodiscard]] bool matchesVfsFilter(const VfsListRequest& request, const VfsEntry& entry);
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_VFS_PROVIDER_HPP
