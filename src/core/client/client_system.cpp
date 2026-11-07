#include "core/client/client.hpp"

#include "core/client/client_transport.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace mpfshost
{
    using detail::connectAndBind;
    using detail::exchange;
    using detail::exchangeStreamed;
    using detail::expectResponse;
    using detail::request;

    std::optional<BindResponse>
    bindTarget(const std::filesystem::path& socketPath, const std::string& targetName, std::string& error)
    {
        const auto payload  = encodeBindRequest({targetName});
        const auto response = request(socketPath, MessageType::BindRequest, payload, error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto responsePayload = expectResponse(*response, MessageType::BindResponse, error);
        if (!responsePayload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeBindResponse(*responsePayload);
        if (!decoded)
        {
            error = "server returned a malformed bind response";
        }
        return decoded;
    }

    std::optional<ListTargetsResponse> listTargets(const std::filesystem::path& socketPath, std::string& error)
    {
        const auto response = request(socketPath, MessageType::ListTargetsRequest, {}, error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::ListTargetsResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeListTargetsResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed list-targets response";
        }
        return decoded;
    }

    std::optional<CapabilitiesResponse> getCapabilities(const std::filesystem::path& socketPath, std::string& error)
    {
        const auto response = request(socketPath, MessageType::CapabilitiesRequest, {}, error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::CapabilitiesResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeCapabilitiesResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed capabilities response";
        }
        return decoded;
    }

    std::optional<ServiceListAnswer>
    listServices(const std::filesystem::path& socketPath, const ServiceQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::ServiceListRequest,
                                               encodeServiceListRequest({query.filter}),
                                               MessageType::ServiceListEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodeServiceListEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed service list end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "service listing failed" : end->error;
            return std::nullopt;
        }

        ServiceListAnswer answer;
        answer.truncated = end->truncated != 0;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodeServiceListData(frame);
            if (!decoded)
            {
                error = "server returned malformed service list data";
                return std::nullopt;
            }
            answer.services.insert(answer.services.end(), decoded->services.begin(), decoded->services.end());
        }
        if (answer.services.size() != end->serviceCount)
        {
            error = "server reported " + std::to_string(end->serviceCount) + " services but delivered "
                  + std::to_string(answer.services.size());
            return std::nullopt;
        }
        return answer;
    }

    std::optional<UserListAnswer>
    listUsers(const std::filesystem::path& socketPath, const UserQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::UserListRequest,
                                               encodeUserListRequest({query.filter}),
                                               MessageType::UserListEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodeUserListEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed user list end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "user listing failed" : end->error;
            return std::nullopt;
        }

        UserListAnswer answer;
        answer.truncated = end->truncated != 0;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodeUserListData(frame);
            if (!decoded)
            {
                error = "server returned malformed user list data";
                return std::nullopt;
            }
            answer.users.insert(answer.users.end(), decoded->users.begin(), decoded->users.end());
        }
        if (answer.users.size() != end->userCount)
        {
            error = "server reported " + std::to_string(end->userCount) + " users but delivered "
                  + std::to_string(answer.users.size());
            return std::nullopt;
        }
        return answer;
    }

    std::optional<NetworkListAnswer>
    listNetwork(const std::filesystem::path& socketPath, const NetworkQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::NetworkListRequest,
                                               encodeNetworkListRequest({query.pid, query.filter}),
                                               MessageType::NetworkListEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodeNetworkListEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed network list end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "connection listing failed" : end->error;
            return std::nullopt;
        }

        NetworkListAnswer answer;
        answer.truncated = end->truncated != 0;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodeNetworkListData(frame);
            if (!decoded)
            {
                error = "server returned malformed network list data";
                return std::nullopt;
            }
            answer.entries.insert(answer.entries.end(), decoded->entries.begin(), decoded->entries.end());
        }
        if (answer.entries.size() != end->entryCount)
        {
            error = "server reported " + std::to_string(end->entryCount) + " connections but delivered "
                  + std::to_string(answer.entries.size());
            return std::nullopt;
        }
        return answer;
    }

    std::optional<HeapListAnswer>
    listHeaps(const std::filesystem::path& socketPath, const HeapQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::HeapListRequest,
                                               encodeHeapListRequest({query.pid}),
                                               MessageType::HeapListEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodeHeapListEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed heap list end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "heap listing failed" : end->error;
            return std::nullopt;
        }

        HeapListAnswer answer;
        answer.truncated = end->truncated != 0;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodeHeapListData(frame);
            if (!decoded)
            {
                error = "server returned malformed heap list data";
                return std::nullopt;
            }
            answer.heaps.insert(answer.heaps.end(), decoded->heaps.begin(), decoded->heaps.end());
        }
        if (answer.heaps.size() != end->heapCount)
        {
            error = "server reported " + std::to_string(end->heapCount) + " heaps but delivered "
                  + std::to_string(answer.heaps.size());
            return std::nullopt;
        }
        return answer;
    }

    std::optional<HeapAllocListAnswer>
    listHeapAllocations(const std::filesystem::path& socketPath, const HeapAllocQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::HeapAllocListRequest,
                                               encodeHeapAllocListRequest({query.pid, query.heap}),
                                               MessageType::HeapAllocListEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodeHeapAllocListEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed heap allocation list end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "heap allocation listing failed" : end->error;
            return std::nullopt;
        }

        HeapAllocListAnswer answer;
        answer.truncated = end->truncated != 0;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodeHeapAllocListData(frame);
            if (!decoded)
            {
                error = "server returned malformed heap allocation list data";
                return std::nullopt;
            }
            answer.allocations.insert(
                answer.allocations.end(), decoded->allocations.begin(), decoded->allocations.end());
        }
        if (answer.allocations.size() != end->allocationCount)
        {
            error = "server reported " + std::to_string(end->allocationCount) + " allocations but delivered "
                  + std::to_string(answer.allocations.size());
            return std::nullopt;
        }
        return answer;
    }

    std::optional<std::vector<VmDescriptor>>
    listVms(const std::filesystem::path& socketPath, const std::string& targetName, std::string& error)
    {
        const auto connection = connectAndBind(socketPath, targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto response = exchange(connection->fd(), MessageType::VmListRequest, {}, error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::VmListResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeVmListResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed vm list response";
            return std::nullopt;
        }
        return decoded->vms;
    }

    std::optional<std::vector<std::byte>>
    readPhysicalMemory(const std::filesystem::path& socketPath, const PhysicalReadQuery& query, std::string& error)
    {
        error.clear();
        if (query.size == 0)
        {
            error = "read size must be at least one byte";
            return std::nullopt;
        }
        if (query.size > std::numeric_limits<std::uint64_t>::max() - query.gpa)
        {
            error = "read range overflows the guest address space";
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
            const auto payload  = encodePhysicalReadRequest({query.vmIndex, query.gpa + offset, chunk});
            const auto streamed = exchangeStreamed(
                connection->fd(), MessageType::PhysicalReadRequest, payload, MessageType::PhysicalReadEnd, error);
            if (!streamed)
            {
                return std::nullopt;
            }

            const auto end = decodePhysicalReadEnd(streamed->endPayload);
            if (!end)
            {
                error = "server returned a malformed physical read end";
                return std::nullopt;
            }
            if (end->status != 0)
            {
                error = end->error.empty() ? "guest physical memory read failed" : end->error;
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

    std::optional<std::uint64_t> writePhysicalMemory(const std::filesystem::path& socketPath,
                                                     const PhysicalWriteCommand&  command,
                                                     std::string&                 error)
    {
        error.clear();
        if (command.data.empty())
        {
            error = "write payload is empty";
            return std::nullopt;
        }
        if (command.data.size() > std::numeric_limits<std::uint64_t>::max() - command.gpa)
        {
            error = "write range overflows the guest address space";
            return std::nullopt;
        }

        const auto connection = connectAndBind(socketPath, command.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        constexpr std::size_t chunkLimit = kMaxPayloadLength - 16; // vm index, gpa and the data length field
        std::uint64_t         total      = 0;
        std::size_t           offset     = 0;
        while (offset < command.data.size())
        {
            const auto             chunk = std::min(chunkLimit, command.data.size() - offset);
            std::vector<std::byte> slice(command.data.begin() + static_cast<std::ptrdiff_t>(offset),
                                         command.data.begin() + static_cast<std::ptrdiff_t>(offset + chunk));

            const auto payload  = encodePhysicalWriteRequest({command.vmIndex, command.gpa + offset, std::move(slice)});
            const auto response = exchange(connection->fd(), MessageType::PhysicalWriteRequest, payload, error);
            if (!response)
            {
                return std::nullopt;
            }

            const auto responsePayload = expectResponse(*response, MessageType::PhysicalWriteResponse, error);
            if (!responsePayload)
            {
                return std::nullopt;
            }

            const auto decoded = decodeMemoryWriteResponse(*responsePayload);
            if (!decoded)
            {
                error = "server returned a malformed physical write response";
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

    std::optional<GpaTranslateResponse>
    translateGpa(const std::filesystem::path& socketPath, const GpaTranslateQuery& query, std::string& error)
    {
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto payload  = encodeGpaTranslateRequest({query.vmIndex, query.gpa});
        const auto response = exchange(connection->fd(), MessageType::GpaTranslateRequest, payload, error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto responsePayload = expectResponse(*response, MessageType::GpaTranslateResponse, error);
        if (!responsePayload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeGpaTranslateResponse(*responsePayload);
        if (!decoded)
        {
            error = "server returned a malformed gpa translate response";
        }
        return decoded;
    }

    std::optional<std::uint64_t>
    virt2Phys(const std::filesystem::path& socketPath, const Virt2PhysQuery& query, std::string& error)
    {
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto payload  = encodeVirt2PhysRequest({query.pid, query.va});
        const auto response = exchange(connection->fd(), MessageType::Virt2PhysRequest, payload, error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto responsePayload = expectResponse(*response, MessageType::Virt2PhysResponse, error);
        if (!responsePayload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeVirt2PhysResponse(*responsePayload);
        if (!decoded)
        {
            error = "server returned a malformed virt2phys response";
            return std::nullopt;
        }
        return decoded->pa;
    }

    std::optional<std::vector<ScatterReadResult>>
    physicalScatter(const std::filesystem::path& socketPath, const PhysicalScatterQuery& query, std::string& error)
    {
        error.clear();
        if (query.entries.empty())
        {
            error = "physical scatter needs at least one range";
            return std::nullopt;
        }
        for (const auto& entry : query.entries)
        {
            if (entry.size == 0)
            {
                error = "physical scatter range size must be at least one byte";
                return std::nullopt;
            }
        }

        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        PhysicalScatterRequest request;
        request.vmIndex = query.vmIndex;
        request.entries.reserve(query.entries.size());
        for (const auto& entry : query.entries)
        {
            request.entries.push_back(
                {entry.address, entry.size, entry.write ? kScatterEntryWrite : kScatterEntryRead, entry.data});
        }

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::PhysicalScatterRequest,
                                               encodePhysicalScatterRequest(request),
                                               MessageType::PhysicalScatterEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodeScatterExecuteEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed physical scatter end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "physical scatter failed" : end->error;
            return std::nullopt;
        }

        std::vector<std::vector<std::byte>> data(end->outcomes.size());
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodeScatterExecuteData(frame);
            if (!decoded)
            {
                error = "server returned malformed physical scatter data";
                return std::nullopt;
            }
            if (decoded->entryIndex >= data.size())
            {
                error = "server returned an out-of-range physical scatter entry index";
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
            results.push_back(
                {outcome.address, outcome.requestedSize, outcome.bytesRead, outcome.ok != 0, std::move(data[index])});
        }
        return results;
    }

    std::optional<std::uint64_t>
    setConfig(const std::filesystem::path& socketPath, const ConfigSetQuery& query, std::string& error)
    {
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto request = exchange(connection->fd(),
                                      MessageType::ConfigSetRequest,
                                      encodeConfigSetRequest({query.option, query.pid, query.value}),
                                      error);
        if (!request)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*request, MessageType::ConfigSetResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeConfigSetResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed config set response";
            return std::nullopt;
        }
        return decoded->value;
    }
} // namespace mpfshost
