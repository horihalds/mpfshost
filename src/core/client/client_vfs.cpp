#include "core/client/client_legacy.hpp"

#include "core/client/client_transport.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace mpfshost
{
    using detail::connectAndBind;
    using detail::exchange;
    using detail::exchangeStreamed;
    using detail::expectResponse;

    std::optional<std::vector<VfsEntry>>
    listVfs(const std::filesystem::path& socketPath, const VfsQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::VfsListRequest,
                                               encodeVfsListRequest({query.path, query.nameFilter}),
                                               MessageType::VfsListEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodeVfsListEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed vfs list end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "vfs listing failed" : end->error;
            return std::nullopt;
        }

        std::vector<VfsEntry> entries;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodeVfsListData(frame);
            if (!decoded)
            {
                error = "server returned malformed vfs list data";
                return std::nullopt;
            }
            entries.insert(entries.end(), decoded->entries.begin(), decoded->entries.end());
        }
        if (entries.size() != end->entryCount)
        {
            error = "server reported " + std::to_string(end->entryCount) + " vfs entries but delivered "
                  + std::to_string(entries.size());
            return std::nullopt;
        }
        return entries;
    }

    std::optional<std::vector<std::byte>>
    readVfs(const std::filesystem::path& socketPath, const VfsReadQuery& query, std::string& error)
    {
        error.clear();
        if (query.size == 0)
        {
            error = "read size must be at least one byte";
            return std::nullopt;
        }
        if (query.size > kMaxReadSize)
        {
            error = "read size exceeds the maximum of " + std::to_string(kMaxReadSize) + " bytes";
            return std::nullopt;
        }

        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::VfsReadRequest,
                                               encodeVfsReadRequest({query.path, query.offset, query.size}),
                                               MessageType::VfsReadEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodeVfsReadEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed vfs read end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "vfs read failed" : end->error;
            return std::nullopt;
        }

        std::vector<std::byte> bytes;
        for (const auto& frame : streamed->data)
        {
            bytes.insert(bytes.end(), frame.begin(), frame.end());
        }
        if (bytes.size() != end->bytesRead)
        {
            error = "server reported " + std::to_string(end->bytesRead) + " vfs bytes but delivered "
                  + std::to_string(bytes.size());
            return std::nullopt;
        }
        if (bytes.size() > query.size)
        {
            error = "server delivered more vfs bytes than requested";
            return std::nullopt;
        }
        return bytes;
    }

    std::optional<std::uint64_t>
    writeVfs(const std::filesystem::path& socketPath, const VfsWriteCommand& command, std::string& error)
    {
        error.clear();
        if (command.data.empty())
        {
            error = "write payload is empty";
            return std::nullopt;
        }
        if (command.data.size() > std::numeric_limits<std::uint64_t>::max() - command.offset)
        {
            error = "vfs write range overflows the offset space";
            return std::nullopt;
        }

        // path string length, offset and the data length field all live in the same single-frame payload.
        const std::size_t overhead = 4 + command.path.size() + 8 + 4;
        if (overhead >= kMaxPayloadLength)
        {
            error = "vfs path is too long";
            return std::nullopt;
        }
        const std::size_t chunkLimit = kMaxPayloadLength - overhead;

        const auto connection = connectAndBind(socketPath, command.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        std::uint64_t total  = 0;
        std::size_t   offset = 0;
        while (offset < command.data.size())
        {
            const auto             chunk = std::min(chunkLimit, command.data.size() - offset);
            std::vector<std::byte> slice(command.data.begin() + static_cast<std::ptrdiff_t>(offset),
                                         command.data.begin() + static_cast<std::ptrdiff_t>(offset + chunk));

            const auto payload  = encodeVfsWriteRequest({command.path, command.offset + offset, std::move(slice)});
            const auto response = exchange(connection->fd(), MessageType::VfsWriteRequest, payload, error);
            if (!response)
            {
                return std::nullopt;
            }

            const auto responsePayload = expectResponse(*response, MessageType::VfsWriteResponse, error);
            if (!responsePayload)
            {
                return std::nullopt;
            }

            const auto decoded = decodeVfsWriteResponse(*responsePayload);
            if (!decoded)
            {
                error = "server returned a malformed vfs write response";
                return std::nullopt;
            }
            if (decoded->bytesWritten != chunk)
            {
                error = "server reported a short vfs write";
                return std::nullopt;
            }

            total += decoded->bytesWritten;
            offset += chunk;
        }
        return total;
    }

} // namespace mpfshost
