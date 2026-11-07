#include "core/server/session.hpp"

#include "core/ipc/message.hpp"
#include "core/log/log.hpp"
#include "core/server/backend.hpp"
#include "core/server/map_batching.hpp"
#include "core/server/memory_provider.hpp"
#include "core/server/memory_search_provider.hpp"
#include "core/server/page_provider.hpp"
#include "core/server/virtual_query_provider.hpp"
#include "core/util/hex.hpp"
#include "core/util/page_range.hpp"

#include <algorithm>
#include <limits>
#include <vector>

namespace mpfshost
{
    namespace
    {
        // A range of zero bytes or one whose end wraps the address space cannot name any page.
        bool isValidPageRanges(std::span<const PageRange> ranges)
        {
            if (ranges.empty())
            {
                return false;
            }
            for (const auto& range : ranges)
            {
                if (range.size == 0 || range.size > std::numeric_limits<std::uint64_t>::max() - range.address)
                {
                    return false;
                }
            }
            return true;
        }

        std::uint64_t pageItemEncodedSize(const PageReadItem& page)
        {
            return sizeof(std::uint64_t) + sizeof(std::uint8_t) + sizeof(std::uint32_t) + page.data.size();
        }

        std::uint64_t searchHitEncodedSize(std::uint64_t /*address*/)
        {
            return sizeof(std::uint64_t);
        }

        // MemProcFS reads the alignment as a power-of-two byte count.
        bool isValidSearchAlignment(std::uint32_t alignBytes)
        {
            return alignBytes != 0 && alignBytes <= kMaxSearchAlignBytes && (alignBytes & (alignBytes - 1)) == 0;
        }
    } // namespace

    void Session::handleVirtualQuery(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeVirtualQueryRequest(payload);
        if (!request.has_value() || request->pid == 0)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed virtual query request");
            return;
        }

        auto* provider = dynamic_cast<VirtualQueryProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot query virtual memory");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        const auto   region = provider->queryVirtual(*request, status, error);
        if (!region.has_value())
        {
            if (status == LookupStatus::NotFound)
            {
                sendError(requestId, ErrorCode::NotFound, "no region covers that address");
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "virtual query failed" : error);
            return;
        }

        sendPayload(requestId, MessageType::VirtualQueryResponse, encodeVirtualQueryResponse({*region}));
    }

    void Session::handlePageRead(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodePageReadRequest(payload);
        if (!request.has_value() || request->pid == 0)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed page read request");
            return;
        }
        if (!isValidPageRanges(request->ranges))
        {
            sendError(requestId, ErrorCode::InvalidRange, "page read range is empty or wraps the address space");
            return;
        }

        const auto addresses = expandPageAddresses(request->ranges, kMaxPageCount);
        if (!addresses.has_value())
        {
            sendError(requestId, ErrorCode::ResultTooLarge, "page read expands above the page count cap");
            return;
        }

        auto* provider = dynamic_cast<PageReadProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot read whole pages");
            return;
        }

        std::vector<PageReadItem> pages;
        std::string               error;
        if (!provider->readPages(request->pid, *addresses, pages, error))
        {
            if (_deadline.expired())
            {
                abortRequest(requestId, ErrorCode::Timeout);
                return;
            }
            const PageReadEnd end {1, 0, error.empty() ? "page read failed" : error};
            sendPayload(requestId, MessageType::PageReadEnd, encodePageReadEnd(end));
            return;
        }

        for (std::size_t offset = 0; offset < pages.size();)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto   count = batchCount(pages, offset, kMaxMapBatchBytes, pageItemEncodedSize);
            PageReadData data;
            data.pages.assign(pages.begin() + static_cast<std::ptrdiff_t>(offset),
                              pages.begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::PageReadData, encodePageReadData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const PageReadEnd end {0, static_cast<std::uint32_t>(pages.size()), ""};
        sendPayload(requestId, MessageType::PageReadEnd, encodePageReadEnd(end));
    }

    void Session::handleMemoryPrefetch(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodePrefetchRequest(payload);
        if (!request.has_value() || request->pid == 0)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed prefetch request");
            return;
        }
        if (!isValidPageRanges(request->ranges))
        {
            sendError(requestId, ErrorCode::InvalidRange, "prefetch range is empty or wraps the address space");
            return;
        }

        const auto addresses = expandPageAddresses(request->ranges, kMaxPrefetchPages);
        if (!addresses.has_value())
        {
            sendError(requestId, ErrorCode::ResultTooLarge, "prefetch expands above the page count cap");
            return;
        }

        auto* provider = dynamic_cast<PrefetchProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot prefetch pages");
            return;
        }

        std::string error;
        if (!provider->prefetchPages(request->pid, *addresses, error))
        {
            if (_deadline.expired())
            {
                abortRequest(requestId, ErrorCode::Timeout);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "prefetch failed" : error);
            return;
        }

        sendPayload(requestId,
                    MessageType::PrefetchResponse,
                    encodePrefetchResponse({static_cast<std::uint32_t>(addresses->size())}));
    }

    void Session::handleMemorySearch(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeMemorySearchRequest(payload);
        if (!request.has_value() || request->pid == 0)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed memory search request");
            return;
        }
        if (!isValidSearchAlignment(request->alignBytes))
        {
            sendError(requestId, ErrorCode::MalformedPayload, "search alignment must be a power of two up to 4096");
            return;
        }
        if (!isSearchableTerm(request->term))
        {
            // VMMDLL_MemSearch compares the raw pattern bytes against zero and fails the whole call when they are
            // all zero, wildcards included; refuse that shape instead of reporting it as an Internal error.
            sendError(requestId, ErrorCode::MalformedPayload, "a search pattern needs at least one non-zero byte");
            return;
        }
        if (request->maxResults == 0)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "a search needs at least one result");
            return;
        }
        if (request->maxResults > kMaxSearchHits)
        {
            sendError(requestId, ErrorCode::ResultTooLarge, "search result count is above the cap");
            return;
        }
        if (request->maxAddress != 0 && request->maxAddress <= request->minAddress)
        {
            sendError(requestId, ErrorCode::InvalidRange, "search window must end above its start");
            return;
        }

        auto* provider = dynamic_cast<MemorySearchProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot search memory");
            return;
        }

        std::vector<std::uint64_t> hits;
        bool                       truncated = false;
        std::string                error;
        if (!provider->searchMemory(*request, hits, truncated, error))
        {
            if (_deadline.expired())
            {
                abortRequest(requestId, ErrorCode::Timeout);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "memory search failed" : error);
            return;
        }

        for (std::size_t offset = 0; offset < hits.size();)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto       count = batchCount(hits, offset, kMaxMapBatchBytes, searchHitEncodedSize);
            MemorySearchData data;
            data.addresses.assign(hits.begin() + static_cast<std::ptrdiff_t>(offset),
                                  hits.begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::MemorySearchData, encodeMemorySearchData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const MemorySearchEnd end {
            0, static_cast<std::uint32_t>(hits.size()), static_cast<std::uint8_t>(truncated ? 1 : 0), ""};
        sendPayload(requestId, MessageType::MemorySearchEnd, encodeMemorySearchEnd(end));
    }

    void Session::handleMemoryRead(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeMemoryReadRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed memory read request");
            return;
        }
        if (!isValidMemoryRange(request->address, request->size))
        {
            sendError(requestId, ErrorCode::InvalidRange, "memory read range is invalid");
            return;
        }

        auto* provider = dynamic_cast<MemoryProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot read memory");
            return;
        }

        std::vector<std::byte> bytes;
        std::string            error;
        if (!provider->readMemory(request->pid, request->address, request->size, bytes, error))
        {
            const MemoryReadEnd end {1, 0, error.empty() ? "memory read failed" : error};
            sendPayload(requestId, MessageType::MemoryReadEnd, encodeMemoryReadEnd(end));
            return;
        }
        if (bytes.size() != request->size)
        {
            const MemoryReadEnd end {1,
                                     0,
                                     "memory provider returned " + std::to_string(bytes.size()) + " of "
                                         + std::to_string(request->size) + " bytes"};
            sendPayload(requestId, MessageType::MemoryReadEnd, encodeMemoryReadEnd(end));
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
            sendFrame(
                MessageType::MemoryReadData, requestId, std::span<const std::byte> {bytes.data() + offset, chunk});
            if (_writeFailed)
            {
                return;
            }
        }

        const MemoryReadEnd end {0, bytes.size(), ""};
        sendPayload(requestId, MessageType::MemoryReadEnd, encodeMemoryReadEnd(end));
    }

    void Session::handleMemoryWrite(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeMemoryWriteRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed memory write request");
            return;
        }
        if (request->data.empty()
            || !isValidMemoryRange(request->address, static_cast<std::uint32_t>(request->data.size())))
        {
            sendError(requestId, ErrorCode::InvalidRange, "memory write range is invalid");
            return;
        }

        auto* provider = dynamic_cast<MemoryProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot write memory");
            return;
        }

        std::string error;
        if (!provider->writeMemory(request->pid, request->address, request->data, error))
        {
            sendError(requestId, ErrorCode::Internal, error.empty() ? "memory write failed" : error);
            return;
        }

        sendPayload(requestId,
                    MessageType::MemoryWriteResponse,
                    encodeMemoryWriteResponse({static_cast<std::uint32_t>(request->data.size())}));
    }
} // namespace mpfshost
