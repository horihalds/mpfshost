#include "core/server/session.hpp"

#include "core/ipc/message.hpp"
#include "core/log/log.hpp"
#include "core/server/backend.hpp"
#include "core/server/kernel_device_provider.hpp"
#include "core/server/kernel_driver_provider.hpp"
#include "core/server/kernel_object_provider.hpp"
#include "core/server/map_batching.hpp"
#include "core/server/pfn_provider.hpp"
#include "core/server/pool_provider.hpp"
#include "core/server/pte_provider.hpp"

#include <vector>

namespace mpfshost
{
    void Session::handleKDriverList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeKDriverListRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed kernel driver list request");
            return;
        }

        auto* provider = dynamic_cast<KernelDriverProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list kernel drivers");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        auto         result = provider->listKernelDrivers(*request, status, error);
        if (!result.has_value())
        {
            sendError(requestId,
                      status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                      error.empty() ? "kernel driver listing failed" : error);
            return;
        }

        for (std::size_t offset = 0; offset < result->drivers.size();)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto      count = batchCount(result->drivers, offset, kMaxMapBatchBytes, kernelDriverEncodedSize);
            KDriverListData data;
            data.drivers.assign(result->drivers.begin() + static_cast<std::ptrdiff_t>(offset),
                                result->drivers.begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::KDriverListData, encodeKDriverListData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const KDriverListEnd end {0,
                                  static_cast<std::uint64_t>(result->drivers.size()),
                                  static_cast<std::uint8_t>(result->truncated ? 1 : 0),
                                  ""};
        sendPayload(requestId, MessageType::KDriverListEnd, encodeKDriverListEnd(end));
    }

    void Session::handleKDeviceList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeKDeviceListRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed kernel device list request");
            return;
        }

        auto* provider = dynamic_cast<KernelDeviceProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list kernel devices");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        auto         result = provider->listKernelDevices(*request, status, error);
        if (!result.has_value())
        {
            sendError(requestId,
                      status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                      error.empty() ? "kernel device listing failed" : error);
            return;
        }

        for (std::size_t offset = 0; offset < result->devices.size();)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto      count = batchCount(result->devices, offset, kMaxMapBatchBytes, kernelDeviceEncodedSize);
            KDeviceListData data;
            data.devices.assign(result->devices.begin() + static_cast<std::ptrdiff_t>(offset),
                                result->devices.begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::KDeviceListData, encodeKDeviceListData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const KDeviceListEnd end {0,
                                  static_cast<std::uint64_t>(result->devices.size()),
                                  static_cast<std::uint8_t>(result->truncated ? 1 : 0),
                                  ""};
        sendPayload(requestId, MessageType::KDeviceListEnd, encodeKDeviceListEnd(end));
    }

    void Session::handleKObjectList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeKObjectListRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed kernel object list request");
            return;
        }

        auto* provider = dynamic_cast<KernelObjectProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list kernel objects");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        auto         result = provider->listKernelObjects(*request, status, error);
        if (!result.has_value())
        {
            sendError(requestId,
                      status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                      error.empty() ? "kernel object listing failed" : error);
            return;
        }

        for (std::size_t offset = 0; offset < result->objects.size();)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto      count = batchCount(result->objects, offset, kMaxMapBatchBytes, kernelObjectEncodedSize);
            KObjectListData data;
            data.objects.assign(result->objects.begin() + static_cast<std::ptrdiff_t>(offset),
                                result->objects.begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::KObjectListData, encodeKObjectListData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const KObjectListEnd end {0,
                                  static_cast<std::uint64_t>(result->objects.size()),
                                  static_cast<std::uint8_t>(result->truncated ? 1 : 0),
                                  ""};
        sendPayload(requestId, MessageType::KObjectListEnd, encodeKObjectListEnd(end));
    }

    void Session::handlePteList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodePteListRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed pte list request");
            return;
        }

        auto* provider = dynamic_cast<PteProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list ptes");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        auto         result = provider->listPte(*request, status, error);
        if (!result.has_value())
        {
            if (_deadline.expired())
            {
                abortRequest(requestId, ErrorCode::Timeout);
                return;
            }
            sendError(requestId,
                      status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                      error.empty() ? "pte listing failed" : error);
            return;
        }

        for (std::size_t offset = 0; offset < result->entries.size();)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto  count = batchCount(result->entries, offset, kMaxMapBatchBytes, pteEncodedSize);
            PteListData data;
            data.entries.assign(result->entries.begin() + static_cast<std::ptrdiff_t>(offset),
                                result->entries.begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::PteListData, encodePteListData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const PteListEnd end {0,
                              static_cast<std::uint64_t>(result->entries.size()),
                              result->mapEntries,
                              static_cast<std::uint8_t>(result->truncated ? 1 : 0),
                              ""};
        sendPayload(requestId, MessageType::PteListEnd, encodePteListEnd(end));
    }

    void Session::handlePoolList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodePoolListRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed pool list request");
            return;
        }

        auto* provider = dynamic_cast<PoolProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list the pool");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        auto         result = provider->listPool(*request, status, error);
        if (!result.has_value())
        {
            if (_deadline.expired())
            {
                abortRequest(requestId, ErrorCode::Timeout);
                return;
            }
            sendError(requestId,
                      status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                      error.empty() ? "pool listing failed" : error);
            return;
        }

        for (std::size_t offset = 0; offset < result->entries.size();)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto   count = batchCount(result->entries, offset, kMaxMapBatchBytes, poolEncodedSize);
            PoolListData data;
            data.entries.assign(result->entries.begin() + static_cast<std::ptrdiff_t>(offset),
                                result->entries.begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::PoolListData, encodePoolListData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const PoolListEnd end {0,
                               static_cast<std::uint64_t>(result->entries.size()),
                               result->mapEntries,
                               result->mapBytes,
                               static_cast<std::uint8_t>(result->truncated ? 1 : 0),
                               ""};
        sendPayload(requestId, MessageType::PoolListEnd, encodePoolListEnd(end));
    }

    void Session::handlePoolTagList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodePoolTagListRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed pool tag list request");
            return;
        }
        if (request->tagFilter.size() > 4)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "pool tag filter is longer than four characters");
            return;
        }

        auto* provider = dynamic_cast<PoolProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot summarize the pool tags");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        auto         result = provider->listPoolTags(*request, status, error);
        if (!result.has_value())
        {
            if (_deadline.expired())
            {
                abortRequest(requestId, ErrorCode::Timeout);
                return;
            }
            sendError(requestId,
                      status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                      error.empty() ? "pool tag listing failed" : error);
            return;
        }

        for (std::size_t offset = 0; offset < result->tags.size();)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto      count = batchCount(result->tags, offset, kMaxMapBatchBytes, poolTagEncodedSize);
            PoolTagListData data;
            data.tags.assign(result->tags.begin() + static_cast<std::ptrdiff_t>(offset),
                             result->tags.begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::PoolTagListData, encodePoolTagListData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const PoolTagListEnd end {0,
                                  static_cast<std::uint64_t>(result->tags.size()),
                                  result->mapTags,
                                  result->mapEntries,
                                  static_cast<std::uint8_t>(result->truncated ? 1 : 0),
                                  ""};
        sendPayload(requestId, MessageType::PoolTagListEnd, encodePoolTagListEnd(end));
    }

    void Session::handlePfnList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodePfnListRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed pfn list request");
            return;
        }
        if (request->pfnCount == 0 || request->pfnCount > kMaxPfnEntries
            || static_cast<std::uint64_t>(request->pfnStart) + request->pfnCount > 0xFFFFFFFFULL + 1)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "pfn range is out of bounds");
            return;
        }

        auto* provider = dynamic_cast<PfnProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot describe pfns");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        auto         result = provider->listPfn(*request, status, error);
        if (!result.has_value())
        {
            if (_deadline.expired())
            {
                abortRequest(requestId, ErrorCode::Timeout);
                return;
            }
            sendError(requestId,
                      status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                      error.empty() ? "pfn listing failed" : error);
            return;
        }

        for (std::size_t offset = 0; offset < result->entries.size();)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto  count = batchCount(result->entries, offset, kMaxMapBatchBytes, pfnEncodedSize);
            PfnListData data;
            data.entries.assign(result->entries.begin() + static_cast<std::ptrdiff_t>(offset),
                                result->entries.begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::PfnListData, encodePfnListData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const PfnListEnd end {0, static_cast<std::uint64_t>(result->entries.size()), ""};
        sendPayload(requestId, MessageType::PfnListEnd, encodePfnListEnd(end));
    }
} // namespace mpfshost
