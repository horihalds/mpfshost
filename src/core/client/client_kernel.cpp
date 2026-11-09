#include "core/client/client_legacy.hpp"

#include "core/client/client_transport.hpp"

namespace mpfshost
{
    using detail::connectAndBind;
    using detail::exchangeStreamed;
    using detail::expectResponse;

    std::optional<KernelDriverAnswer>
    listKernelDrivers(const std::filesystem::path& socketPath, const KernelDriverQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::KDriverListRequest,
                                               encodeKDriverListRequest({query.filter}),
                                               MessageType::KDriverListEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodeKDriverListEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed kernel driver list end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "kernel driver listing failed" : end->error;
            return std::nullopt;
        }

        KernelDriverAnswer answer;
        answer.truncated = end->truncated != 0;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodeKDriverListData(frame);
            if (!decoded)
            {
                error = "server returned malformed kernel driver list data";
                return std::nullopt;
            }
            answer.drivers.insert(answer.drivers.end(), decoded->drivers.begin(), decoded->drivers.end());
        }
        if (answer.drivers.size() != end->driverCount)
        {
            error = "server reported " + std::to_string(end->driverCount) + " kernel drivers but delivered "
                  + std::to_string(answer.drivers.size());
            return std::nullopt;
        }
        return answer;
    }

    std::optional<KernelDeviceAnswer>
    listKernelDevices(const std::filesystem::path& socketPath, const KernelDeviceQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::KDeviceListRequest,
                                               encodeKDeviceListRequest({query.filter}),
                                               MessageType::KDeviceListEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodeKDeviceListEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed kernel device list end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "kernel device listing failed" : end->error;
            return std::nullopt;
        }

        KernelDeviceAnswer answer;
        answer.truncated = end->truncated != 0;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodeKDeviceListData(frame);
            if (!decoded)
            {
                error = "server returned malformed kernel device list data";
                return std::nullopt;
            }
            answer.devices.insert(answer.devices.end(), decoded->devices.begin(), decoded->devices.end());
        }
        if (answer.devices.size() != end->deviceCount)
        {
            error = "server reported " + std::to_string(end->deviceCount) + " kernel devices but delivered "
                  + std::to_string(answer.devices.size());
            return std::nullopt;
        }
        return answer;
    }

    std::optional<KernelObjectAnswer>
    listKernelObjects(const std::filesystem::path& socketPath, const KernelObjectQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::KObjectListRequest,
                                               encodeKObjectListRequest({query.filter}),
                                               MessageType::KObjectListEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodeKObjectListEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed kernel object list end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "kernel object listing failed" : end->error;
            return std::nullopt;
        }

        KernelObjectAnswer answer;
        answer.truncated = end->truncated != 0;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodeKObjectListData(frame);
            if (!decoded)
            {
                error = "server returned malformed kernel object list data";
                return std::nullopt;
            }
            answer.objects.insert(answer.objects.end(), decoded->objects.begin(), decoded->objects.end());
        }
        if (answer.objects.size() != end->objectCount)
        {
            error = "server reported " + std::to_string(end->objectCount) + " kernel objects but delivered "
                  + std::to_string(answer.objects.size());
            return std::nullopt;
        }
        return answer;
    }

    std::optional<PteAnswer> listPte(const std::filesystem::path& socketPath, const PteQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        PteListRequest request;
        request.pid             = query.pid;
        request.identifyModules = query.identifyModules ? 1 : 0;
        request.minAddress      = query.minAddress;
        request.maxAddress      = query.maxAddress;
        request.filter          = query.filter;

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::PteListRequest,
                                               encodePteListRequest(request),
                                               MessageType::PteListEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodePteListEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed pte list end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "pte listing failed" : end->error;
            return std::nullopt;
        }

        PteAnswer answer;
        answer.mapEntries = end->mapEntries;
        answer.truncated  = end->truncated != 0;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodePteListData(frame);
            if (!decoded)
            {
                error = "server returned malformed pte list data";
                return std::nullopt;
            }
            answer.regions.insert(answer.regions.end(), decoded->entries.begin(), decoded->entries.end());
        }
        if (answer.regions.size() != end->entryCount)
        {
            error = "server reported " + std::to_string(end->entryCount) + " pte regions but delivered "
                  + std::to_string(answer.regions.size());
            return std::nullopt;
        }
        return answer;
    }

    std::optional<PoolAnswer>
    listPool(const std::filesystem::path& socketPath, const PoolQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        PoolListRequest request;
        request.big        = query.big ? 1 : 0;
        request.tagFilter  = query.tagFilter;
        request.minBytes   = query.minBytes;
        request.minAddress = query.minAddress;
        request.maxAddress = query.maxAddress;

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::PoolListRequest,
                                               encodePoolListRequest(request),
                                               MessageType::PoolListEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodePoolListEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed pool list end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "pool listing failed" : end->error;
            return std::nullopt;
        }

        PoolAnswer answer;
        answer.mapEntries = end->mapEntries;
        answer.mapBytes   = end->mapBytes;
        answer.truncated  = end->truncated != 0;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodePoolListData(frame);
            if (!decoded)
            {
                error = "server returned malformed pool list data";
                return std::nullopt;
            }
            answer.entries.insert(answer.entries.end(), decoded->entries.begin(), decoded->entries.end());
        }
        if (answer.entries.size() != end->entryCount)
        {
            error = "server reported " + std::to_string(end->entryCount) + " pool entries but delivered "
                  + std::to_string(answer.entries.size());
            return std::nullopt;
        }
        return answer;
    }

    std::optional<PoolTagAnswer>
    listPoolTags(const std::filesystem::path& socketPath, const PoolTagQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        PoolTagListRequest request;
        request.big       = query.big ? 1 : 0;
        request.tagFilter = query.tagFilter;

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::PoolTagListRequest,
                                               encodePoolTagListRequest(request),
                                               MessageType::PoolTagListEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodePoolTagListEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed pool tag list end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "pool tag listing failed" : end->error;
            return std::nullopt;
        }

        PoolTagAnswer answer;
        answer.mapTags    = end->mapTags;
        answer.mapEntries = end->mapEntries;
        answer.truncated  = end->truncated != 0;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodePoolTagListData(frame);
            if (!decoded)
            {
                error = "server returned malformed pool tag list data";
                return std::nullopt;
            }
            answer.tags.insert(answer.tags.end(), decoded->tags.begin(), decoded->tags.end());
        }
        if (answer.tags.size() != end->entryCount)
        {
            error = "server reported " + std::to_string(end->entryCount) + " pool tags but delivered "
                  + std::to_string(answer.tags.size());
            return std::nullopt;
        }
        return answer;
    }

    std::optional<PfnAnswer> listPfn(const std::filesystem::path& socketPath, const PfnQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const PfnListRequest request {query.pfnStart, query.pfnCount};
        const auto           streamed = exchangeStreamed(connection->fd(),
                                                         MessageType::PfnListRequest,
                                                         encodePfnListRequest(request),
                                                         MessageType::PfnListEnd,
                                                         error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodePfnListEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed pfn list end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "pfn listing failed" : end->error;
            return std::nullopt;
        }

        PfnAnswer answer;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodePfnListData(frame);
            if (!decoded)
            {
                error = "server returned malformed pfn list data";
                return std::nullopt;
            }
            answer.entries.insert(answer.entries.end(), decoded->entries.begin(), decoded->entries.end());
        }
        if (answer.entries.size() != end->entryCount)
        {
            error = "server reported " + std::to_string(end->entryCount) + " pfns but delivered "
                  + std::to_string(answer.entries.size());
            return std::nullopt;
        }
        return answer;
    }

} // namespace mpfshost
