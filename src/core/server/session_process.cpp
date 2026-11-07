#include "core/server/session.hpp"

#include "core/ipc/message.hpp"
#include "core/log/log.hpp"
#include "core/server/backend.hpp"
#include "core/server/handle_provider.hpp"
#include "core/server/map_batching.hpp"
#include "core/server/module_lookup_provider.hpp"
#include "core/server/pdb_provider.hpp"
#include "core/server/process_info_provider.hpp"
#include "core/server/process_provider.hpp"

#include <vector>

namespace mpfshost
{
    void Session::handleProcessList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto filter = decodeProcessListRequest(payload);
        if (!filter.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed process list request");
            return;
        }

        auto* provider = dynamic_cast<ProcessProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list processes");
            return;
        }

        std::string error;
        auto        processes = provider->listProcesses(*filter, error);
        if (!processes.has_value())
        {
            sendError(requestId, ErrorCode::Internal, error.empty() ? "process listing failed" : error);
            return;
        }

        sendPayload(requestId, MessageType::ProcessListResponse, encodeProcessListResponse({*processes}));
    }

    void Session::handleProcessListAll(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto filter = decodeProcessListRequest(payload);
        if (!filter.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed process list request");
            return;
        }

        auto* provider = dynamic_cast<ProcessProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list processes");
            return;
        }

        std::string error;
        auto        processes = provider->listAllProcesses(*filter, error);
        if (!processes.has_value())
        {
            sendError(requestId, ErrorCode::Internal, error.empty() ? "process listing failed" : error);
            return;
        }

        sendPayload(requestId, MessageType::ProcessListAllResponse, encodeProcessListAllResponse({*processes}));
    }

    void Session::handleProcessInfo(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeProcessInfoRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed process info request");
            return;
        }

        auto* provider = dynamic_cast<ProcessInfoProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot describe processes");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        if (auto response = provider->processInfo(*request, status, error); response.has_value())
        {
            sendPayload(requestId, MessageType::ProcessInfoResponse, encodeProcessInfoResponse(*response));
            return;
        }

        // "Nothing carries that name" is an expected answer, so it is reported as NotFound rather than as the
        // generic Internal failure the client would otherwise treat as a host fault.
        sendError(requestId,
                  status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                  error.empty() ? "process lookup failed" : error);
    }

    void Session::handleSymbolLookup(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeSymbolLookupRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed symbol lookup request");
            return;
        }

        auto* provider = dynamic_cast<ModuleLookupProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot resolve symbols");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        if (auto response = provider->lookupSymbol(*request, status, error); response.has_value())
        {
            sendPayload(requestId, MessageType::SymbolLookupResponse, encodeSymbolLookupResponse(*response));
            return;
        }

        sendError(requestId,
                  status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                  error.empty() ? "symbol lookup failed" : error);
    }

    void Session::handlePdbLoad(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodePdbLoadRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed pdb load request");
            return;
        }
        if (request->moduleBase == 0)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "a pdb load needs a module base");
            return;
        }

        auto* provider = dynamic_cast<PdbProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot load symbols");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        if (auto response = provider->pdbLoad(*request, status, error); response.has_value())
        {
            sendPayload(requestId, MessageType::PdbLoadResponse, encodePdbLoadResponse(*response));
            return;
        }

        sendError(requestId,
                  status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                  error.empty() ? "pdb load failed" : error);
    }

    void Session::handlePdbSymbolName(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodePdbSymbolNameRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed pdb symbol name request");
            return;
        }
        if (request->module.empty())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "a pdb symbol name lookup needs a module");
            return;
        }

        auto* provider = dynamic_cast<PdbProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot resolve symbols");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        if (auto response = provider->pdbSymbolName(*request, status, error); response.has_value())
        {
            sendPayload(requestId, MessageType::PdbSymbolNameResponse, encodePdbSymbolNameResponse(*response));
            return;
        }

        sendError(requestId,
                  status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                  error.empty() ? "pdb symbol name lookup failed" : error);
    }

    void Session::handlePdbSymbolAddress(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodePdbSymbolAddressRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed pdb symbol address request");
            return;
        }
        if (request->module.empty() || request->symbol.empty())
        {
            sendError(
                requestId, ErrorCode::MalformedPayload, "a pdb symbol address lookup needs a module and a symbol");
            return;
        }

        auto* provider = dynamic_cast<PdbProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot resolve symbols");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        if (auto response = provider->pdbSymbolAddress(*request, status, error); response.has_value())
        {
            sendPayload(requestId, MessageType::PdbSymbolAddressResponse, encodePdbSymbolAddressResponse(*response));
            return;
        }

        sendError(requestId,
                  status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                  error.empty() ? "pdb symbol address lookup failed" : error);
    }

    void Session::handlePdbTypeSize(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodePdbTypeSizeRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed pdb type size request");
            return;
        }
        if (request->module.empty() || request->type.empty())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "a pdb type size lookup needs a module and a type");
            return;
        }

        auto* provider = dynamic_cast<PdbProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot resolve symbols");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        if (auto response = provider->pdbTypeSize(*request, status, error); response.has_value())
        {
            sendPayload(requestId, MessageType::PdbTypeSizeResponse, encodePdbTypeSizeResponse(*response));
            return;
        }

        sendError(requestId,
                  status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                  error.empty() ? "pdb type size lookup failed" : error);
    }

    void Session::handlePdbTypeChildOffset(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodePdbTypeChildOffsetRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed pdb type child offset request");
            return;
        }
        if (request->module.empty() || request->type.empty() || request->child.empty())
        {
            sendError(requestId,
                      ErrorCode::MalformedPayload,
                      "a pdb type child offset lookup needs a module, a type and a child");
            return;
        }

        auto* provider = dynamic_cast<PdbProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot resolve symbols");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        if (auto response = provider->pdbTypeChildOffset(*request, status, error); response.has_value())
        {
            sendPayload(
                requestId, MessageType::PdbTypeChildOffsetResponse, encodePdbTypeChildOffsetResponse(*response));
            return;
        }

        sendError(requestId,
                  status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                  error.empty() ? "pdb type child offset lookup failed" : error);
    }

    void Session::handleHandleList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeHandleListRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed handle list request");
            return;
        }
        if (request->pid == 0)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "handle listing needs a process id");
            return;
        }

        auto* provider = dynamic_cast<HandleProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list handles");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        auto         result = provider->listHandles(*request, status, error);
        if (!result.has_value())
        {
            sendError(requestId,
                      status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                      error.empty() ? "handle listing failed" : error);
            return;
        }

        for (std::size_t offset = 0; offset < result->handles.size();)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto     count = batchCount(result->handles, offset, kMaxMapBatchBytes, handleEncodedSize);
            HandleListData data;
            data.handles.assign(result->handles.begin() + static_cast<std::ptrdiff_t>(offset),
                                result->handles.begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::HandleListData, encodeHandleListData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const HandleListEnd end {0,
                                 static_cast<std::uint64_t>(result->handles.size()),
                                 static_cast<std::uint8_t>(result->truncated ? 1 : 0),
                                 ""};
        sendPayload(requestId, MessageType::HandleListEnd, encodeHandleListEnd(end));
    }
} // namespace mpfshost
