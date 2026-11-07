#include "core/client/client.hpp"

#include "core/client/client_transport.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include <unistd.h>

namespace mpfshost
{
    using detail::connectAndBind;
    using detail::exchange;
    using detail::exchangeStreamed;
    using detail::expectResponse;

    namespace
    {
        std::optional<std::vector<ScatterReadResult>> runScatterExecute(
            int fd, std::uint32_t scatterId, MessageType requestType, MessageType endType, std::string& error)
        {
            const auto requestPayload = requestType == MessageType::ScatterExecuteReadRequest
                                          ? encodeScatterExecuteReadRequest({scatterId})
                                          : encodeScatterExecuteRequest({scatterId});
            const auto streamed       = exchangeStreamed(fd, requestType, requestPayload, endType, error);
            if (!streamed)
            {
                return std::nullopt;
            }

            const auto end = endType == MessageType::ScatterExecuteReadEnd
                               ? decodeScatterExecuteReadEnd(streamed->endPayload)
                               : decodeScatterExecuteEnd(streamed->endPayload);
            if (!end)
            {
                error = "server returned a malformed scatter end";
                return std::nullopt;
            }
            if (end->status != 0)
            {
                error = end->error.empty() ? "scatter execution failed" : end->error;
                return std::nullopt;
            }

            std::vector<std::vector<std::byte>> data(end->outcomes.size());
            for (const auto& frame : streamed->data)
            {
                const auto decoded = decodeScatterExecuteData(frame);
                if (!decoded)
                {
                    error = "server returned malformed scatter data";
                    return std::nullopt;
                }
                if (decoded->entryIndex >= data.size())
                {
                    error = "server returned an out-of-range scatter entry index";
                    return std::nullopt;
                }
                auto& entryData = data[decoded->entryIndex];
                entryData.insert(entryData.end(), decoded->data.begin(), decoded->data.end());
            }

            std::vector<ScatterReadResult> results;
            results.reserve(end->outcomes.size());
            for (std::size_t index = 0; index < end->outcomes.size(); ++index)
            {
                const auto& outcome = end->outcomes[index];
                results.push_back({outcome.address,
                                   outcome.requestedSize,
                                   outcome.bytesRead,
                                   outcome.ok != 0,
                                   std::move(data[index])});
            }
            return results;
        }
    } // namespace

    std::optional<VirtualRegion>
    queryVirtual(const std::filesystem::path& socketPath, const VirtualQueryQuery& query, std::string& error)
    {
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto requestPayload = encodeVirtualQueryRequest({query.pid, query.address});
        const auto request        = exchange(connection->fd(), MessageType::VirtualQueryRequest, requestPayload, error);
        if (!request)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*request, MessageType::VirtualQueryResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeVirtualQueryResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed virtual query response";
            return std::nullopt;
        }
        return decoded->region;
    }

    std::optional<std::vector<PageReadItem>>
    readPages(const std::filesystem::path& socketPath, const PageReadQuery& query, std::string& error)
    {
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::PageReadRequest,
                                               encodePageReadRequest({query.pid, query.ranges}),
                                               MessageType::PageReadEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodePageReadEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed page read end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "page read failed" : end->error;
            return std::nullopt;
        }

        std::vector<PageReadItem> pages;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodePageReadData(frame);
            if (!decoded)
            {
                error = "server returned malformed page read data";
                return std::nullopt;
            }
            pages.insert(pages.end(), decoded->pages.begin(), decoded->pages.end());
        }
        if (pages.size() != end->pageCount)
        {
            error = "server reported " + std::to_string(end->pageCount) + " pages but delivered "
                  + std::to_string(pages.size());
            return std::nullopt;
        }
        return pages;
    }

    std::optional<std::uint32_t>
    prefetchPages(const std::filesystem::path& socketPath, const PrefetchQuery& query, std::string& error)
    {
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto request = exchange(
            connection->fd(), MessageType::PrefetchRequest, encodePrefetchRequest({query.pid, query.ranges}), error);
        if (!request)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*request, MessageType::PrefetchResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodePrefetchResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed prefetch response";
            return std::nullopt;
        }
        return decoded->pageCount;
    }

    std::optional<SearchResult>
    searchMemory(const std::filesystem::path& socketPath, const MemorySearchQuery& query, std::string& error)
    {
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto streamed = exchangeStreamed(
            connection->fd(),
            MessageType::MemorySearchRequest,
            encodeMemorySearchRequest(
                {query.pid, query.term, query.alignBytes, query.minAddress, query.maxAddress, query.maxResults}),
            MessageType::MemorySearchEnd,
            error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodeMemorySearchEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed search end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "memory search failed" : end->error;
            return std::nullopt;
        }

        SearchResult result;
        result.truncated = end->truncated != 0;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodeMemorySearchData(frame);
            if (!decoded)
            {
                error = "server returned malformed search data";
                return std::nullopt;
            }
            result.addresses.insert(result.addresses.end(), decoded->addresses.begin(), decoded->addresses.end());
        }
        if (result.addresses.size() != end->hitCount)
        {
            error = "server reported " + std::to_string(end->hitCount) + " hits but delivered "
                  + std::to_string(result.addresses.size());
            return std::nullopt;
        }
        return result;
    }

    std::optional<std::vector<std::byte>>
    readMemory(const std::filesystem::path& socketPath, const MemoryQuery& query, std::string& error)
    {
        error.clear();
        if (query.size == 0)
        {
            error = "read size must be at least one byte";
            return std::nullopt;
        }
        if (query.size > std::numeric_limits<std::uint64_t>::max() - query.address)
        {
            error = "read range overflows the address space";
            return std::nullopt;
        }

        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        std::vector<std::byte> result;
        result.reserve(query.size);
        std::uint32_t remaining = query.size;
        std::uint64_t offset    = 0;
        while (remaining > 0)
        {
            const auto chunk    = std::min<std::uint32_t>(remaining, kMaxReadSize);
            const auto payload  = encodeMemoryReadRequest({query.pid, query.address + offset, chunk});
            const auto streamed = exchangeStreamed(
                connection->fd(), MessageType::MemoryReadRequest, payload, MessageType::MemoryReadEnd, error);
            if (!streamed)
            {
                return std::nullopt;
            }

            const auto end = decodeMemoryReadEnd(streamed->endPayload);
            if (!end)
            {
                error = "server returned a malformed memory read end";
                return std::nullopt;
            }
            if (end->status != 0)
            {
                error = end->error.empty() ? "memory read failed" : end->error;
                return std::nullopt;
            }

            std::size_t received = 0;
            for (const auto& frame : streamed->data)
            {
                result.insert(result.end(), frame.begin(), frame.end());
                received += frame.size();
            }
            if (received != chunk)
            {
                error = "server returned " + std::to_string(received) + " of " + std::to_string(chunk) + " bytes";
                return std::nullopt;
            }

            offset += chunk;
            remaining -= chunk;
        }
        return result;
    }

    std::optional<std::uint64_t>
    writeMemory(const std::filesystem::path& socketPath, const MemoryWriteCommand& command, std::string& error)
    {
        error.clear();
        if (command.data.empty())
        {
            error = "write payload is empty";
            return std::nullopt;
        }
        if (command.data.size() > std::numeric_limits<std::uint64_t>::max() - command.address)
        {
            error = "write range overflows the address space";
            return std::nullopt;
        }

        const auto connection = connectAndBind(socketPath, command.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        constexpr std::size_t chunkLimit = kMaxPayloadLength - 16; // pid, address and the data length field
        std::uint64_t         total      = 0;
        std::size_t           offset     = 0;
        while (offset < command.data.size())
        {
            const auto             chunk = std::min(chunkLimit, command.data.size() - offset);
            std::vector<std::byte> slice(command.data.begin() + static_cast<std::ptrdiff_t>(offset),
                                         command.data.begin() + static_cast<std::ptrdiff_t>(offset + chunk));

            const auto payload  = encodeMemoryWriteRequest({command.pid, command.address + offset, std::move(slice)});
            const auto response = exchange(connection->fd(), MessageType::MemoryWriteRequest, payload, error);
            if (!response)
            {
                return std::nullopt;
            }

            const auto responsePayload = expectResponse(*response, MessageType::MemoryWriteResponse, error);
            if (!responsePayload)
            {
                return std::nullopt;
            }

            const auto decoded = decodeMemoryWriteResponse(*responsePayload);
            if (!decoded)
            {
                error = "server returned a malformed memory write response";
                return std::nullopt;
            }
            if (decoded->bytesWritten != chunk)
            {
                error = "server reported a short write";
                return std::nullopt;
            }

            total += decoded->bytesWritten;
            offset += chunk;
        }
        return total;
    }

    ScatterSession::ScatterSession(int fd, std::uint32_t scatterId) noexcept : _fd {fd}, _scatterId {scatterId} {}

    ScatterSession::~ScatterSession()
    {
        if (_fd >= 0)
        {
            ::close(_fd);
        }
    }

    ScatterSession::ScatterSession(ScatterSession&& other) noexcept : _fd {other._fd}, _scatterId {other._scatterId}
    {
        other._fd        = -1;
        other._scatterId = 0;
    }

    ScatterSession& ScatterSession::operator=(ScatterSession&& other) noexcept
    {
        if (this != &other)
        {
            if (_fd >= 0)
            {
                ::close(_fd);
            }
            _fd              = other._fd;
            _scatterId       = other._scatterId;
            other._fd        = -1;
            other._scatterId = 0;
        }
        return *this;
    }

    std::uint32_t ScatterSession::scatterId() const noexcept
    {
        return _scatterId;
    }

    std::optional<std::uint32_t> ScatterSession::prepare(const std::vector<ScatterItem>& entries, std::string& error)
    {
        error.clear();
        if (_fd < 0)
        {
            error = "scatter session is closed";
            return std::nullopt;
        }

        ScatterPrepareRequest request;
        request.scatterId = _scatterId;
        request.entries.reserve(entries.size());
        for (const auto& entry : entries)
        {
            request.entries.push_back(
                {entry.address, entry.size, entry.write ? kScatterEntryWrite : kScatterEntryRead, entry.data});
        }

        const auto response =
            exchange(_fd, MessageType::ScatterPrepareRequest, encodeScatterPrepareRequest(request), error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::ScatterPrepareResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeScatterPrepareResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed scatter prepare response";
            return std::nullopt;
        }
        return decoded->entryCount;
    }

    std::optional<std::vector<ScatterReadResult>> ScatterSession::execute(std::string& error)
    {
        error.clear();
        if (_fd < 0)
        {
            error = "scatter session is closed";
            return std::nullopt;
        }

        return runScatterExecute(
            _fd, _scatterId, MessageType::ScatterExecuteRequest, MessageType::ScatterExecuteEnd, error);
    }

    std::optional<std::vector<ScatterReadResult>> ScatterSession::executeRead(std::string& error)
    {
        error.clear();
        if (_fd < 0)
        {
            error = "scatter session is closed";
            return std::nullopt;
        }

        return runScatterExecute(
            _fd, _scatterId, MessageType::ScatterExecuteReadRequest, MessageType::ScatterExecuteReadEnd, error);
    }

    bool ScatterSession::clear(std::string& error)
    {
        error.clear();
        if (_fd < 0)
        {
            error = "scatter session is closed";
            return false;
        }

        const auto response =
            exchange(_fd, MessageType::ScatterClearRequest, encodeScatterClearRequest({_scatterId}), error);
        if (!response)
        {
            return false;
        }

        const auto payload = expectResponse(*response, MessageType::ScatterClearResponse, error);
        if (!payload)
        {
            return false;
        }

        const auto decoded = decodeScatterClearResponse(*payload);
        if (!decoded || decoded->scatterId != _scatterId)
        {
            error = "server returned a malformed scatter clear response";
            return false;
        }
        return true;
    }

    bool ScatterSession::close(std::string& error)
    {
        error.clear();
        if (_fd < 0)
        {
            error = "scatter session is already closed";
            return false;
        }

        const auto response =
            exchange(_fd, MessageType::ScatterCloseRequest, encodeScatterCloseRequest({_scatterId}), error);
        if (!response)
        {
            return false;
        }

        const auto payload = expectResponse(*response, MessageType::ScatterCloseResponse, error);
        if (!payload)
        {
            return false;
        }

        const auto decoded = decodeScatterCloseResponse(*payload);
        if (!decoded || decoded->scatterId != _scatterId)
        {
            error = "server returned a malformed scatter close response";
            return false;
        }

        ::close(_fd);
        _fd        = -1;
        _scatterId = 0;
        return true;
    }

    std::optional<ScatterSession> createScatter(const std::filesystem::path& socketPath,
                                                const std::string&           targetName,
                                                std::uint32_t                pid,
                                                std::string&                 error)
    {
        error.clear();
        auto connection = connectAndBind(socketPath, targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto response =
            exchange(connection->fd(), MessageType::ScatterCreateRequest, encodeScatterCreateRequest({pid}), error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::ScatterCreateResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeScatterCreateResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed scatter create response";
            return std::nullopt;
        }

        return ScatterSession {connection->release(), decoded->scatterId};
    }

    std::optional<std::vector<ScatterReadResult>>
    scatterRead(const std::filesystem::path& socketPath, const ScatterReadQuery& query, std::string& error)
    {
        error.clear();
        if (query.ranges.empty())
        {
            error = "scatter read needs at least one range";
            return std::nullopt;
        }
        for (const auto& range : query.ranges)
        {
            if (range.size == 0)
            {
                error = "scatter range size must be at least one byte";
                return std::nullopt;
            }
        }

        auto session = createScatter(socketPath, query.targetName, query.pid, error);
        if (!session)
        {
            return std::nullopt;
        }

        std::vector<ScatterItem> entries;
        entries.reserve(query.ranges.size());
        for (const auto& range : query.ranges)
        {
            entries.push_back({range.address, range.size, false, {}});
        }

        if (!session->prepare(entries, error))
        {
            return std::nullopt;
        }
        auto results = session->execute(error);
        if (!results)
        {
            return std::nullopt;
        }
        if (!session->close(error))
        {
            return std::nullopt;
        }
        return results;
    }

} // namespace mpfshost
