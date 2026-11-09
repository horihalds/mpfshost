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

    std::optional<std::vector<DebugRegisterRecord>>
    debugRegisters(const std::filesystem::path& socketPath, const DebugRegistersQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto response = exchange(
            connection->fd(), MessageType::DebugRegistersRequest, encodeDebugRegistersRequest({query.vcpu}), error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::DebugRegistersResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeDebugRegistersResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed debug registers response";
            return std::nullopt;
        }
        return decoded->records;
    }

    std::optional<DebugWatchEnd> debugWatch(const std::filesystem::path&                      socketPath,
                                            const DebugWatchQuery&                            query,
                                            const std::function<void(const DebugWatchData&)>& onHit,
                                            std::string&                                      error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto requestId = detail::nextRequestId();
        if (!writeFrame(connection->fd(),
                        MessageType::DebugWatchRequest,
                        requestId,
                        encodeDebugWatchRequest(
                            {query.pid, query.address, query.width, query.access, query.maxHits, query.timeoutMs}),
                        error))
        {
            return std::nullopt;
        }

        while (true)
        {
            FrameHeader            header;
            std::vector<std::byte> payload;
            const auto             status = readFrame(connection->fd(), header, payload, error);
            if (status != FrameReadStatus::Complete)
            {
                if (error.empty())
                {
                    error = status == FrameReadStatus::Timeout ? "the host timed out waiting for this request"
                                                               : "server closed the connection";
                }
                return std::nullopt;
            }
            if (header.requestId != requestId)
            {
                error = "server answered a different request id";
                return std::nullopt;
            }
            if (header.type == MessageType::ErrorResponse)
            {
                if (const auto decoded = decodeErrorResponse(payload))
                {
                    error = decoded->message;
                }
                else
                {
                    error = "server returned a malformed error";
                }
                return std::nullopt;
            }
            if (header.type == MessageType::DebugWatchData)
            {
                const auto decoded = decodeDebugWatchData(payload);
                if (!decoded)
                {
                    error = "server returned malformed debug watch data";
                    return std::nullopt;
                }
                if (onHit)
                {
                    onHit(*decoded);
                }
                continue;
            }
            if (header.type == MessageType::DebugWatchEnd)
            {
                const auto decoded = decodeDebugWatchEnd(payload);
                if (!decoded)
                {
                    error = "server returned a malformed debug watch end";
                    return std::nullopt;
                }
                return decoded;
            }
            error = "server returned an unexpected response";
            return std::nullopt;
        }
    }

    std::optional<DebugBreakpointResponse>
    debugBreakpoint(const std::filesystem::path& socketPath, const DebugBreakpointQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto response =
            exchange(connection->fd(),
                     MessageType::DebugBreakpointRequest,
                     encodeDebugBreakpointRequest({query.pid, query.address, query.kind, query.timeoutMs}),
                     error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::DebugBreakpointResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeDebugBreakpointResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed debug breakpoint response";
        }
        return decoded;
    }

    std::optional<DebugStepResponse>
    debugStep(const std::filesystem::path& socketPath, const DebugStepQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto response =
            exchange(connection->fd(), MessageType::DebugStepRequest, encodeDebugStepRequest({query.vcpu}), error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::DebugStepResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeDebugStepResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed debug step response";
        }
        return decoded;
    }

    std::optional<DebugDetachResponse>
    debugDetach(const std::filesystem::path& socketPath, const DebugQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto response =
            exchange(connection->fd(), MessageType::DebugDetachRequest, encodeDebugDetachRequest({}), error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::DebugDetachResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeDebugDetachResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed debug detach response";
        }
        return decoded;
    }
} // namespace mpfshost
