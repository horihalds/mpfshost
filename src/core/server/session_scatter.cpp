#include "core/server/session.hpp"

#include "core/ipc/message.hpp"
#include "core/log/log.hpp"
#include "core/server/backend.hpp"
#include "core/server/memory_provider.hpp"
#include "core/server/scatter_provider.hpp"

#include <algorithm>
#include <vector>

namespace mpfshost
{
    namespace
    {
        bool isValidScatterEntry(const ScatterEntry& entry)
        {
            if (entry.kind == kScatterEntryRead)
            {
                return entry.data.empty() && isValidMemoryRange(entry.address, entry.size);
            }
            if (entry.kind == kScatterEntryWrite)
            {
                return entry.data.size() == entry.size && isValidMemoryRange(entry.address, entry.size);
            }
            return false;
        }
    } // namespace

    Session::ScatterSession* Session::findScatter(std::uint32_t scatterId)
    {
        const auto iterator = _scatters.find(scatterId);
        return iterator == _scatters.end() ? nullptr : &iterator->second;
    }

    void Session::handleScatterCreate(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeScatterCreateRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed scatter create request");
            return;
        }

        auto* provider = dynamic_cast<ScatterProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot gather scatter reads");
            return;
        }
        if (_scatters.size() >= kMaxScatterHandlesPerSession)
        {
            sendError(requestId, ErrorCode::ScatterLimitReached, "too many scatter handles in this session");
            return;
        }

        std::string error;
        auto        handle = provider->createScatter(request->pid, error);
        if (!handle)
        {
            sendError(requestId, ErrorCode::Internal, error.empty() ? "scatter creation failed" : error);
            return;
        }

        while (_scatters.count(_nextScatterId) != 0)
        {
            ++_nextScatterId;
        }
        const auto scatterId = _nextScatterId++;
        _scatters.emplace(scatterId, ScatterSession {std::move(handle), 0, 0});

        sendPayload(requestId, MessageType::ScatterCreateResponse, encodeScatterCreateResponse({scatterId}));
    }

    void Session::handleScatterPrepare(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeScatterPrepareRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed scatter prepare request");
            return;
        }

        auto* scatter = findScatter(request->scatterId);
        if (scatter == nullptr)
        {
            sendError(requestId, ErrorCode::UnknownScatter, "unknown scatter handle");
            return;
        }
        if (request->entries.size() > kMaxScatterEntries - scatter->entryCount)
        {
            sendError(requestId, ErrorCode::InvalidRange, "too many scatter entries");
            return;
        }

        std::uint64_t additionalReadBytes = 0;
        for (const auto& entry : request->entries)
        {
            if (!isValidScatterEntry(entry))
            {
                sendError(requestId, ErrorCode::InvalidRange, "scatter entry range is invalid");
                return;
            }
            if (entry.kind == kScatterEntryRead)
            {
                additionalReadBytes += entry.size;
            }
        }
        if (additionalReadBytes > kMaxScatterReadBytes - scatter->readBytes)
        {
            sendError(requestId, ErrorCode::InvalidRange, "scatter read bytes exceed the session cap");
            return;
        }

        std::vector<ScatterRequestEntry> entries;
        entries.reserve(request->entries.size());
        for (const auto& entry : request->entries)
        {
            entries.push_back({entry.address, entry.size, entry.kind == kScatterEntryWrite, entry.data});
        }

        std::string error;
        if (!scatter->handle->prepare(entries, error))
        {
            sendError(requestId, ErrorCode::Internal, error.empty() ? "scatter preparation failed" : error);
            return;
        }

        scatter->entryCount += static_cast<std::uint32_t>(entries.size());
        scatter->readBytes += additionalReadBytes;

        sendPayload(
            requestId, MessageType::ScatterPrepareResponse, encodeScatterPrepareResponse({scatter->entryCount}));
    }

    bool Session::streamScatterResults(std::uint64_t                     requestId,
                                       const std::vector<ScatterResult>& results,
                                       MessageType                       dataType)
    {
        constexpr std::size_t kDataFrameOverhead = sizeof(std::uint32_t);
        constexpr std::size_t kMaxDataPerFrame   = kMaxPayloadLength - kDataFrameOverhead;

        for (std::size_t index = 0; index < results.size(); ++index)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return false;
            }
            const auto& data   = results[index].data;
            std::size_t offset = 0;
            do
            {
                const auto                   chunk      = std::min<std::size_t>(kMaxDataPerFrame, data.size() - offset);
                const auto                   entryIndex = static_cast<std::uint32_t>(index);
                const std::vector<std::byte> bytes(data.begin() + static_cast<std::ptrdiff_t>(offset),
                                                   data.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
                if (dataType == MessageType::ScatterExecuteReadData)
                {
                    sendPayload(requestId, dataType, encodeScatterExecuteReadData({entryIndex, bytes}));
                }
                else
                {
                    sendPayload(requestId, dataType, encodeScatterExecuteData({entryIndex, bytes}));
                }
                if (_writeFailed)
                {
                    return false;
                }
                offset += chunk;
            }
            while (offset < data.size());
        }
        return true;
    }

    void Session::handleScatterExecute(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeScatterExecuteRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed scatter execute request");
            return;
        }

        auto* scatter = findScatter(request->scatterId);
        if (scatter == nullptr)
        {
            sendError(requestId, ErrorCode::UnknownScatter, "unknown scatter handle");
            return;
        }

        std::vector<ScatterResult> results;
        std::string                error;
        if (!scatter->handle->execute(results, error))
        {
            const ScatterExecuteEnd end {1, {}, error.empty() ? "scatter execution failed" : error};
            sendPayload(requestId, MessageType::ScatterExecuteEnd, encodeScatterExecuteEnd(end));
            return;
        }

        if (!streamScatterResults(requestId, results, MessageType::ScatterExecuteData))
        {
            return;
        }

        ScatterExecuteEnd end {0, {}, ""};
        end.outcomes.reserve(results.size());
        for (const auto& result : results)
        {
            end.outcomes.push_back(
                {result.address, result.requestedSize, result.bytesRead, static_cast<std::uint8_t>(result.ok ? 1 : 0)});
        }
        sendPayload(requestId, MessageType::ScatterExecuteEnd, encodeScatterExecuteEnd(end));
    }

    void Session::handleScatterExecuteRead(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeScatterExecuteReadRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed scatter execute read request");
            return;
        }

        auto* scatter = findScatter(request->scatterId);
        if (scatter == nullptr)
        {
            sendError(requestId, ErrorCode::UnknownScatter, "unknown scatter handle");
            return;
        }

        std::vector<ScatterResult> results;
        std::string                error;
        if (!scatter->handle->executeRead(results, error))
        {
            const ScatterExecuteReadEnd end {1, {}, error.empty() ? "scatter read failed" : error};
            sendPayload(requestId, MessageType::ScatterExecuteReadEnd, encodeScatterExecuteReadEnd(end));
            return;
        }

        if (!streamScatterResults(requestId, results, MessageType::ScatterExecuteReadData))
        {
            return;
        }

        ScatterExecuteReadEnd end {0, {}, ""};
        end.outcomes.reserve(results.size());
        for (const auto& result : results)
        {
            end.outcomes.push_back(
                {result.address, result.requestedSize, result.bytesRead, static_cast<std::uint8_t>(result.ok ? 1 : 0)});
        }
        sendPayload(requestId, MessageType::ScatterExecuteReadEnd, encodeScatterExecuteReadEnd(end));
    }

    void Session::handleScatterClear(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeScatterClearRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed scatter clear request");
            return;
        }

        auto* scatter = findScatter(request->scatterId);
        if (scatter == nullptr)
        {
            sendError(requestId, ErrorCode::UnknownScatter, "unknown scatter handle");
            return;
        }

        std::string error;
        if (!scatter->handle->clear(error))
        {
            sendError(requestId, ErrorCode::Internal, error.empty() ? "scatter clear failed" : error);
            return;
        }

        scatter->entryCount = 0;
        scatter->readBytes  = 0;

        sendPayload(requestId, MessageType::ScatterClearResponse, encodeScatterClearResponse({request->scatterId}));
    }

    void Session::handleScatterClose(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeScatterCloseRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed scatter close request");
            return;
        }

        if (findScatter(request->scatterId) == nullptr)
        {
            sendError(requestId, ErrorCode::UnknownScatter, "unknown scatter handle");
            return;
        }
        _scatters.erase(request->scatterId);

        sendPayload(requestId, MessageType::ScatterCloseResponse, encodeScatterCloseResponse({request->scatterId}));
    }
} // namespace mpfshost
