#include "core/client/client.hpp"

#include "core/client/client_transport.hpp"

namespace mpfshost
{
    using detail::connectAndBind;
    using detail::exchange;
    using detail::exchangeStreamed;
    using detail::expectResponse;

    std::optional<ProcessListResponse>
    listProcesses(const std::filesystem::path& socketPath, const ProcessQuery& query, std::string& error)
    {
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto listPayload  = encodeProcessListRequest({query.pid, query.nameFilter});
        const auto listResponse = exchange(connection->fd(), MessageType::ProcessListRequest, listPayload, error);
        if (!listResponse)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*listResponse, MessageType::ProcessListResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeProcessListResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed process-list response";
        }
        return decoded;
    }

    std::optional<ProcessListAllResponse>
    listAllProcesses(const std::filesystem::path& socketPath, const ProcessQuery& query, std::string& error)
    {
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto listPayload  = encodeProcessListRequest({query.pid, query.nameFilter});
        const auto listResponse = exchange(connection->fd(), MessageType::ProcessListAllRequest, listPayload, error);
        if (!listResponse)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*listResponse, MessageType::ProcessListAllResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeProcessListAllResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed process-list response";
        }
        return decoded;
    }

    std::optional<ProcessInfoResponse>
    processInfo(const std::filesystem::path& socketPath, const ProcessInfoQuery& query, std::string& error)
    {
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto requestPayload = encodeProcessInfoRequest({query.pid, query.name});
        const auto request        = exchange(connection->fd(), MessageType::ProcessInfoRequest, requestPayload, error);
        if (!request)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*request, MessageType::ProcessInfoResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeProcessInfoResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed process-info response";
        }
        return decoded;
    }

    std::optional<ProcessByDtbResponse>
    processByDtb(const std::filesystem::path& socketPath, const ProcessByDtbQuery& query, std::string& error)
    {
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto requestPayload = encodeProcessByDtbRequest({query.dtb});
        const auto request        = exchange(connection->fd(), MessageType::ProcessByDtbRequest, requestPayload, error);
        if (!request)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*request, MessageType::ProcessByDtbResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeProcessByDtbResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed process-by-dtb response";
        }
        return decoded;
    }

    std::optional<SymbolLookupResponse>
    lookupSymbol(const std::filesystem::path& socketPath, const SymbolQuery& query, std::string& error)
    {
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto requestPayload = encodeSymbolLookupRequest({query.pid, query.module, query.symbol});
        const auto request        = exchange(connection->fd(), MessageType::SymbolLookupRequest, requestPayload, error);
        if (!request)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*request, MessageType::SymbolLookupResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeSymbolLookupResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed symbol lookup response";
        }
        return decoded;
    }

    std::optional<PdbLoadResponse>
    pdbLoad(const std::filesystem::path& socketPath, const PdbLoadQuery& query, std::string& error)
    {
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto requestPayload = encodePdbLoadRequest({query.pid, query.moduleBase});
        const auto request        = exchange(connection->fd(), MessageType::PdbLoadRequest, requestPayload, error);
        if (!request)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*request, MessageType::PdbLoadResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodePdbLoadResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed pdb load response";
        }
        return decoded;
    }

    std::optional<PdbSymbolNameResponse>
    pdbSymbolName(const std::filesystem::path& socketPath, const PdbSymbolNameQuery& query, std::string& error)
    {
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto requestPayload = encodePdbSymbolNameRequest({query.module, query.address});
        const auto request = exchange(connection->fd(), MessageType::PdbSymbolNameRequest, requestPayload, error);
        if (!request)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*request, MessageType::PdbSymbolNameResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodePdbSymbolNameResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed pdb symbol name response";
        }
        return decoded;
    }

    std::optional<PdbSymbolAddressResponse>
    pdbSymbolAddress(const std::filesystem::path& socketPath, const PdbSymbolAddressQuery& query, std::string& error)
    {
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto requestPayload = encodePdbSymbolAddressRequest({query.module, query.symbol});
        const auto request = exchange(connection->fd(), MessageType::PdbSymbolAddressRequest, requestPayload, error);
        if (!request)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*request, MessageType::PdbSymbolAddressResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodePdbSymbolAddressResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed pdb symbol address response";
        }
        return decoded;
    }

    std::optional<PdbTypeSizeResponse>
    pdbTypeSize(const std::filesystem::path& socketPath, const PdbTypeSizeQuery& query, std::string& error)
    {
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto requestPayload = encodePdbTypeSizeRequest({query.module, query.type});
        const auto request        = exchange(connection->fd(), MessageType::PdbTypeSizeRequest, requestPayload, error);
        if (!request)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*request, MessageType::PdbTypeSizeResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodePdbTypeSizeResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed pdb type size response";
        }
        return decoded;
    }

    std::optional<PdbTypeChildOffsetResponse> pdbTypeChildOffset(const std::filesystem::path&   socketPath,
                                                                 const PdbTypeChildOffsetQuery& query,
                                                                 std::string&                   error)
    {
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto requestPayload = encodePdbTypeChildOffsetRequest({query.module, query.type, query.child});
        const auto request = exchange(connection->fd(), MessageType::PdbTypeChildOffsetRequest, requestPayload, error);
        if (!request)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*request, MessageType::PdbTypeChildOffsetResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodePdbTypeChildOffsetResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed pdb type child offset response";
        }
        return decoded;
    }

    std::optional<std::vector<HandleDescriptor>>
    listHandles(const std::filesystem::path& socketPath, const HandleQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::HandleListRequest,
                                               encodeHandleListRequest({query.pid, query.filter}),
                                               MessageType::HandleListEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodeHandleListEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed handle list end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "handle listing failed" : end->error;
            return std::nullopt;
        }

        std::vector<HandleDescriptor> handles;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodeHandleListData(frame);
            if (!decoded)
            {
                error = "server returned malformed handle list data";
                return std::nullopt;
            }
            handles.insert(handles.end(), decoded->handles.begin(), decoded->handles.end());
        }
        if (handles.size() != end->handleCount)
        {
            error = "server reported " + std::to_string(end->handleCount) + " handles but delivered "
                  + std::to_string(handles.size());
            return std::nullopt;
        }
        return handles;
    }

} // namespace mpfshost
