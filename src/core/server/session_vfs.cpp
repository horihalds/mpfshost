#include "core/server/session.hpp"

#include "core/ipc/message.hpp"
#include "core/log/log.hpp"
#include "core/server/backend.hpp"
#include "core/server/map_batching.hpp"
#include "core/server/vfs_provider.hpp"

#include <algorithm>
#include <vector>

namespace mpfshost
{
    void Session::handleVfsList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeVfsListRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed vfs list request");
            return;
        }

        auto* provider = dynamic_cast<VfsProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list the vfs");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        auto         entries = provider->listVfs(*request, status, error);
        if (!entries.has_value())
        {
            sendError(requestId,
                      status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                      error.empty() ? "vfs listing failed" : error);
            return;
        }

        for (std::size_t offset = 0; offset < entries->size();)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto  count = batchCount(*entries, offset, kMaxMapBatchBytes, vfsEntryEncodedSize);
            VfsListData data;
            data.entries.assign(entries->begin() + static_cast<std::ptrdiff_t>(offset),
                                entries->begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::VfsListData, encodeVfsListData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const VfsListEnd end {0, entries->size(), ""};
        sendPayload(requestId, MessageType::VfsListEnd, encodeVfsListEnd(end));
    }

    void Session::handleVfsRead(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeVfsReadRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed vfs read request");
            return;
        }
        if (request->size == 0 || request->size > kMaxReadSize)
        {
            sendError(requestId, ErrorCode::InvalidRange, "vfs read size is invalid");
            return;
        }

        auto* provider = dynamic_cast<VfsProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot read the vfs");
            return;
        }

        std::vector<std::byte> bytes;
        std::string            error;
        if (!provider->readVfs(*request, bytes, error))
        {
            const VfsReadEnd end {1, 0, error.empty() ? "vfs read failed" : error};
            sendPayload(requestId, MessageType::VfsReadEnd, encodeVfsReadEnd(end));
            return;
        }
        if (bytes.size() > request->size)
        {
            const VfsReadEnd end {1, 0, "vfs provider returned more bytes than requested"};
            sendPayload(requestId, MessageType::VfsReadEnd, encodeVfsReadEnd(end));
            return;
        }

        for (std::size_t offset = 0; offset < bytes.size(); offset += kMaxPayloadLength)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto chunk = std::min<std::size_t>(kMaxPayloadLength, bytes.size() - offset);
            sendFrame(MessageType::VfsReadData, requestId, std::span<const std::byte> {bytes.data() + offset, chunk});
            if (_writeFailed)
            {
                return;
            }
        }

        const VfsReadEnd end {0, bytes.size(), ""};
        sendPayload(requestId, MessageType::VfsReadEnd, encodeVfsReadEnd(end));
    }

    void Session::handleVfsWrite(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeVfsWriteRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed vfs write request");
            return;
        }
        if (request->data.empty())
        {
            sendError(requestId, ErrorCode::InvalidRange, "vfs write size is invalid");
            return;
        }
        if (!_allowVfsWrite)
        {
            sendError(
                requestId, ErrorCode::AccessDenied, "vfs write is disabled; start the host with --allow-vfs-write");
            return;
        }

        auto* provider = dynamic_cast<VfsProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot write the vfs");
            return;
        }

        std::uint32_t bytesWritten = 0;
        std::string   error;
        if (!provider->writeVfs(*request, bytesWritten, error))
        {
            sendError(requestId, ErrorCode::Internal, error.empty() ? "vfs write failed" : error);
            return;
        }
        if (bytesWritten != request->data.size())
        {
            sendError(requestId, ErrorCode::Internal, "vfs write was short");
            return;
        }

        sendPayload(requestId, MessageType::VfsWriteResponse, encodeVfsWriteResponse({bytesWritten}));
    }
} // namespace mpfshost
