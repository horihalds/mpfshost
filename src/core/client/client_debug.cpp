#include "core/client/client.hpp"

#include "core/client/client_transport.hpp"

namespace mpfshost
{
    using detail::connectAndBind;
    using detail::exchange;
    using detail::expectResponse;

    std::optional<DebugStatus>
    debugState(const std::filesystem::path& socketPath, const DebugQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto response = exchange(connection->fd(), MessageType::DebugStateRequest, {}, error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::DebugStateResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeDebugStateResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed debug state response";
        }
        return decoded;
    }

    std::optional<DebugStatus> debugControl(const std::filesystem::path& socketPath,
                                            const DebugQuery&            query,
                                            DebugControlAction           action,
                                            std::string&                 error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto response =
            exchange(connection->fd(), MessageType::DebugControlRequest, encodeDebugControlRequest({action}), error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::DebugControlResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeDebugControlResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed debug control response";
        }
        return decoded;
    }
} // namespace mpfshost
