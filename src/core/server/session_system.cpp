#include "core/server/session.hpp"

#include "core/ipc/message.hpp"
#include "core/log/log.hpp"
#include "core/server/backend.hpp"
#include "core/server/config_provider.hpp"
#include "core/server/heap_alloc_provider.hpp"
#include "core/server/heap_provider.hpp"
#include "core/server/map_batching.hpp"
#include "core/server/memory_provider.hpp"
#include "core/server/network_provider.hpp"
#include "core/server/physical_memory_provider.hpp"
#include "core/server/service_provider.hpp"
#include "core/server/user_provider.hpp"
#include "core/server/vm_provider.hpp"

#include <algorithm>
#include <vector>

namespace mpfshost
{
    namespace
    {
        std::string unknownVmMessage(std::uint32_t vmIndex)
        {
            return "VM partition " + std::to_string(vmIndex) + " does not exist";
        }
    } // namespace

    void Session::handleServiceList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeServiceListRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed service list request");
            return;
        }

        auto* provider = dynamic_cast<ServiceProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list services");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        auto         result = provider->listServices(*request, status, error);
        if (!result.has_value())
        {
            sendError(requestId,
                      status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                      error.empty() ? "service listing failed" : error);
            return;
        }

        for (std::size_t offset = 0; offset < result->services.size();)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto      count = batchCount(result->services, offset, kMaxMapBatchBytes, serviceEncodedSize);
            ServiceListData data;
            data.services.assign(result->services.begin() + static_cast<std::ptrdiff_t>(offset),
                                 result->services.begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::ServiceListData, encodeServiceListData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const ServiceListEnd end {0,
                                  static_cast<std::uint64_t>(result->services.size()),
                                  static_cast<std::uint8_t>(result->truncated ? 1 : 0),
                                  ""};
        sendPayload(requestId, MessageType::ServiceListEnd, encodeServiceListEnd(end));
    }

    void Session::handleUserList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeUserListRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed user list request");
            return;
        }

        auto* provider = dynamic_cast<UserProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list users");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        auto         result = provider->listUsers(*request, status, error);
        if (!result.has_value())
        {
            sendError(requestId,
                      status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                      error.empty() ? "user listing failed" : error);
            return;
        }

        for (std::size_t offset = 0; offset < result->users.size();)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto   count = batchCount(result->users, offset, kMaxMapBatchBytes, userEncodedSize);
            UserListData data;
            data.users.assign(result->users.begin() + static_cast<std::ptrdiff_t>(offset),
                              result->users.begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::UserListData, encodeUserListData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const UserListEnd end {0,
                               static_cast<std::uint64_t>(result->users.size()),
                               static_cast<std::uint8_t>(result->truncated ? 1 : 0),
                               ""};
        sendPayload(requestId, MessageType::UserListEnd, encodeUserListEnd(end));
    }

    void Session::handleNetworkList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeNetworkListRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed network list request");
            return;
        }

        auto* provider = dynamic_cast<NetworkProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list connections");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        auto         result = provider->listNetwork(*request, status, error);
        if (!result.has_value())
        {
            sendError(requestId,
                      status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                      error.empty() ? "connection listing failed" : error);
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
            const auto      count = batchCount(result->entries, offset, kMaxMapBatchBytes, networkEntryEncodedSize);
            NetworkListData data;
            data.entries.assign(result->entries.begin() + static_cast<std::ptrdiff_t>(offset),
                                result->entries.begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::NetworkListData, encodeNetworkListData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const NetworkListEnd end {0,
                                  static_cast<std::uint64_t>(result->entries.size()),
                                  static_cast<std::uint8_t>(result->truncated ? 1 : 0),
                                  ""};
        sendPayload(requestId, MessageType::NetworkListEnd, encodeNetworkListEnd(end));
    }

    void Session::handleHeapList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeHeapListRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed heap list request");
            return;
        }

        auto* provider = dynamic_cast<HeapProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list heaps");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        auto         result = provider->listHeaps(*request, status, error);
        if (!result.has_value())
        {
            sendError(requestId,
                      status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                      error.empty() ? "heap listing failed" : error);
            return;
        }

        for (std::size_t offset = 0; offset < result->heaps.size();)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto   count = batchCount(result->heaps, offset, kMaxMapBatchBytes, heapEncodedSize);
            HeapListData data;
            data.heaps.assign(result->heaps.begin() + static_cast<std::ptrdiff_t>(offset),
                              result->heaps.begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::HeapListData, encodeHeapListData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const HeapListEnd end {0,
                               static_cast<std::uint64_t>(result->heaps.size()),
                               static_cast<std::uint8_t>(result->truncated ? 1 : 0),
                               ""};
        sendPayload(requestId, MessageType::HeapListEnd, encodeHeapListEnd(end));
    }

    void Session::handleHeapAllocList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeHeapAllocListRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed heap allocation list request");
            return;
        }

        auto* provider = dynamic_cast<HeapAllocProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list heap allocations");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        auto         result = provider->listHeapAllocations(*request, status, error);
        if (!result.has_value())
        {
            sendError(requestId,
                      status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                      error.empty() ? "heap allocation listing failed" : error);
            return;
        }

        for (std::size_t offset = 0; offset < result->allocations.size();)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto        count = batchCount(result->allocations, offset, kMaxMapBatchBytes, heapAllocEncodedSize);
            HeapAllocListData data;
            data.allocations.assign(result->allocations.begin() + static_cast<std::ptrdiff_t>(offset),
                                    result->allocations.begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::HeapAllocListData, encodeHeapAllocListData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const HeapAllocListEnd end {0,
                                    static_cast<std::uint64_t>(result->allocations.size()),
                                    static_cast<std::uint8_t>(result->truncated ? 1 : 0),
                                    ""};
        sendPayload(requestId, MessageType::HeapAllocListEnd, encodeHeapAllocListEnd(end));
    }

    std::optional<std::vector<VmDescriptor>> Session::loadVms(std::uint64_t requestId)
    {
        auto* provider = dynamic_cast<VmProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list VM partitions");
            return std::nullopt;
        }

        std::string error;
        auto        vms = provider->listVms(error);
        if (!vms.has_value())
        {
            sendError(requestId, ErrorCode::Internal, error.empty() ? "VM partition listing failed" : error);
            return std::nullopt;
        }
        return vms;
    }

    void Session::handleVmList(std::uint64_t requestId)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto vms = loadVms(requestId);
        if (!vms.has_value())
        {
            return;
        }

        sendPayload(requestId, MessageType::VmListResponse, encodeVmListResponse({*vms}));
    }

    void Session::handleGpaTranslate(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeGpaTranslateRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed gpa translate request");
            return;
        }

        const auto vms = loadVms(requestId);
        if (!vms.has_value())
        {
            return;
        }

        const auto* vm = findVm(*vms, request->vmIndex);
        if (vm == nullptr)
        {
            sendError(requestId, ErrorCode::UnknownVm, unknownVmMessage(request->vmIndex));
            return;
        }
        if (request->gpa >= vm->gpaMax)
        {
            sendError(requestId, ErrorCode::InvalidRange, "gpa translate address is outside the VM partition");
            return;
        }

        auto* provider = dynamic_cast<PhysicalMemoryProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot translate guest addresses");
            return;
        }

        GpaTranslateResponse response;
        std::string          error;
        if (!provider->translateGpa(request->vmIndex, request->gpa, response, error))
        {
            sendError(requestId, ErrorCode::Internal, error.empty() ? "gpa translation failed" : error);
            return;
        }

        sendPayload(requestId, MessageType::GpaTranslateResponse, encodeGpaTranslateResponse(response));
    }

    void Session::handleVirt2Phys(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeVirt2PhysRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed virt2phys request");
            return;
        }

        auto* provider = dynamic_cast<MemoryProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot translate addresses");
            return;
        }

        std::uint64_t pa {};
        std::string   error;
        if (!provider->virt2Phys(request->pid, request->va, pa, error))
        {
            sendError(requestId, ErrorCode::Internal, error.empty() ? "virtual to physical translation failed" : error);
            return;
        }

        sendPayload(requestId, MessageType::Virt2PhysResponse, encodeVirt2PhysResponse({pa}));
    }

    void Session::handlePhysicalRead(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodePhysicalReadRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed physical read request");
            return;
        }
        if (!isValidMemoryRange(request->gpa, request->size))
        {
            sendError(requestId, ErrorCode::InvalidRange, "physical read range is invalid");
            return;
        }

        const auto vms = loadVms(requestId);
        if (!vms.has_value())
        {
            return;
        }

        const auto* vm = findVm(*vms, request->vmIndex);
        if (vm == nullptr)
        {
            sendError(requestId, ErrorCode::UnknownVm, unknownVmMessage(request->vmIndex));
            return;
        }
        if (!isWithinPhysicalRange(request->gpa, request->size, vm->gpaMax))
        {
            sendError(requestId, ErrorCode::InvalidRange, "physical read range leaves the VM partition");
            return;
        }

        auto* provider = dynamic_cast<PhysicalMemoryProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(
                requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot read guest physical memory");
            return;
        }

        std::vector<std::byte> bytes;
        std::string            error;
        std::uint64_t          delivered = 0;
        if (provider->readPhysical(request->vmIndex, request->gpa, request->size, bytes, error))
        {
            delivered = bytes.size();
            if (bytes.size() == request->size)
            {
                for (std::size_t offset = 0; offset < bytes.size(); offset += kMaxPayloadLength)
                {
                    ErrorCode abortCode {};
                    if (requestAborted(requestId, abortCode))
                    {
                        abortRequest(requestId, abortCode);
                        return;
                    }
                    const auto chunk = std::min<std::size_t>(kMaxPayloadLength, bytes.size() - offset);
                    sendFrame(MessageType::PhysicalReadData,
                              requestId,
                              std::span<const std::byte> {bytes.data() + offset, chunk});
                    if (_writeFailed)
                    {
                        return;
                    }
                }

                const PhysicalReadEnd end {0, delivered, ""};
                sendPayload(requestId, MessageType::PhysicalReadEnd, encodePhysicalReadEnd(end));
                return;
            }
            error = "guest physical memory provider returned " + std::to_string(bytes.size()) + " of "
                  + std::to_string(request->size) + " bytes";
        }
        if (error.empty())
        {
            error = "guest physical memory read failed";
        }

        const PhysicalReadEnd end {1, 0, error};
        sendPayload(requestId, MessageType::PhysicalReadEnd, encodePhysicalReadEnd(end));
    }

    void Session::handlePhysicalWrite(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodePhysicalWriteRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed physical write request");
            return;
        }
        if (request->data.empty()
            || !isValidMemoryRange(request->gpa, static_cast<std::uint32_t>(request->data.size())))
        {
            sendError(requestId, ErrorCode::InvalidRange, "physical write range is invalid");
            return;
        }

        const auto vms = loadVms(requestId);
        if (!vms.has_value())
        {
            return;
        }

        const auto* vm = findVm(*vms, request->vmIndex);
        if (vm == nullptr)
        {
            sendError(requestId, ErrorCode::UnknownVm, unknownVmMessage(request->vmIndex));
            return;
        }
        if (!isWithinPhysicalRange(request->gpa, request->data.size(), vm->gpaMax))
        {
            sendError(requestId, ErrorCode::InvalidRange, "physical write range leaves the VM partition");
            return;
        }

        auto* provider = dynamic_cast<PhysicalMemoryProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(
                requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot write guest physical memory");
            return;
        }

        std::string error;
        if (!provider->writePhysical(request->vmIndex, request->gpa, request->data, error))
        {
            sendError(requestId, ErrorCode::Internal, error.empty() ? "guest physical memory write failed" : error);
            return;
        }

        sendPayload(requestId,
                    MessageType::PhysicalWriteResponse,
                    encodeMemoryWriteResponse({static_cast<std::uint32_t>(request->data.size())}));
    }

    void Session::handlePhysicalScatter(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodePhysicalScatterRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed physical scatter request");
            return;
        }
        if (request->entries.empty() || request->entries.size() > kMaxPhysicalScatterEntries)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "physical scatter batch size is out of range");
            return;
        }

        std::uint64_t totalBytes = 0;
        for (const auto& entry : request->entries)
        {
            if (!isValidPhysicalScatterEntry(entry))
            {
                sendError(
                    requestId, ErrorCode::MalformedPayload, "physical scatter entry violates the MEM_SCATTER rules");
                return;
            }
            totalBytes += entry.size;
        }
        if (totalBytes > kMaxPhysicalScatterBytes)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "physical scatter batch exceeds the byte limit");
            return;
        }

        const auto vms = loadVms(requestId);
        if (!vms.has_value())
        {
            return;
        }

        const auto* vm = findVm(*vms, request->vmIndex);
        if (vm == nullptr)
        {
            sendError(requestId, ErrorCode::UnknownVm, unknownVmMessage(request->vmIndex));
            return;
        }
        for (const auto& entry : request->entries)
        {
            if (!isWithinPhysicalRange(entry.address, entry.size, vm->gpaMax))
            {
                sendError(requestId, ErrorCode::InvalidRange, "physical scatter entry leaves the VM partition");
                return;
            }
        }

        auto* provider = dynamic_cast<PhysicalMemoryProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(
                requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot scatter guest physical memory");
            return;
        }

        std::vector<ScatterResult> results;
        std::string                error;
        if (!provider->scatterPhysical(request->vmIndex, request->entries, results, error))
        {
            const ScatterExecuteEnd end {1, {}, error.empty() ? "physical scatter failed" : error};
            sendPayload(requestId, MessageType::PhysicalScatterEnd, encodeScatterExecuteEnd(end));
            return;
        }

        constexpr std::size_t kDataFrameOverhead = sizeof(std::uint32_t);
        constexpr std::size_t kMaxDataPerFrame   = kMaxPayloadLength - kDataFrameOverhead;

        for (std::size_t index = 0; index < results.size(); ++index)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            if (!results[index].ok)
            {
                continue;
            }

            const auto& data   = results[index].data;
            std::size_t offset = 0;
            do
            {
                const auto               chunk = std::min<std::size_t>(kMaxDataPerFrame, data.size() - offset);
                const ScatterExecuteData frame {
                    static_cast<std::uint32_t>(index),
                    std::vector<std::byte>(data.begin() + static_cast<std::ptrdiff_t>(offset),
                                           data.begin() + static_cast<std::ptrdiff_t>(offset + chunk))};
                sendPayload(requestId, MessageType::PhysicalScatterData, encodeScatterExecuteData(frame));
                if (_writeFailed)
                {
                    return;
                }
                offset += chunk;
            }
            while (offset < data.size());
        }

        ScatterExecuteEnd end {0, {}, ""};
        end.outcomes.reserve(results.size());
        for (const auto& result : results)
        {
            end.outcomes.push_back(
                {result.address, result.requestedSize, result.bytesRead, static_cast<std::uint8_t>(result.ok ? 1 : 0)});
        }
        sendPayload(requestId, MessageType::PhysicalScatterEnd, encodeScatterExecuteEnd(end));
    }

    void Session::handleConfigSet(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeConfigSetRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed config set request");
            return;
        }

        const bool usesPid = configOptionUsesPid(request->option);
        if (usesPid && request->pid == 0)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "the config option needs a pid");
            return;
        }
        if (!usesPid && request->pid != 0)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "the config option does not take a pid");
            return;
        }

        const bool isProcessDtb =
            request->option == ConfigOption::ProcessDtb || request->option == ConfigOption::ProcessDtbFastLowIntegrity;
        if (isProcessDtb && (request->value == 0 || (request->value & 0xFFF) != 0))
        {
            sendError(requestId, ErrorCode::InvalidRange, "a process DTB must be a nonzero page-aligned address");
            return;
        }

        auto* provider = dynamic_cast<ConfigProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot set config options");
            return;
        }

        std::string error;
        if (!provider->setConfig(*request, error))
        {
            if (_deadline.expired())
            {
                abortRequest(requestId, ErrorCode::Timeout);
                return;
            }
            sendError(requestId, ErrorCode::Internal, error.empty() ? "config set failed" : error);
            return;
        }

        sendPayload(requestId, MessageType::ConfigSetResponse, encodeConfigSetResponse({request->value}));
    }
} // namespace mpfshost
