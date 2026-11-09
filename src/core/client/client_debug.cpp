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

    std::optional<std::vector<DebugThreadRecord>>
    debugThreads(const std::filesystem::path& socketPath, const DebugThreadsQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto response =
            exchange(connection->fd(), MessageType::DebugThreadsRequest, encodeDebugThreadsRequest({}), error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::DebugThreadsResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeDebugThreadsResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed debug threads response";
            return std::nullopt;
        }
        return decoded->threads;
    }

    std::optional<DebugDisassembleResponse>
    debugDisassemble(const std::filesystem::path& socketPath, const DebugDisassembleQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto response = exchange(connection->fd(),
                                       MessageType::DebugDisassembleRequest,
                                       encodeDebugDisassembleRequest({query.pid, query.address, query.count}),
                                       error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::DebugDisassembleResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeDebugDisassembleResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed debug disassemble response";
        }
        return decoded;
    }

    std::optional<DebugBacktraceResponse>
    debugBacktrace(const std::filesystem::path& socketPath, const DebugBacktraceQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto response =
            exchange(connection->fd(),
                     MessageType::DebugBacktraceRequest,
                     encodeDebugBacktraceRequest({query.pid, query.vcpu, query.threadId, query.maxFrames}),
                     error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::DebugBacktraceResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeDebugBacktraceResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed debug backtrace response";
        }
        return decoded;
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
                        encodeDebugWatchRequest({query.pid,
                                                 query.address,
                                                 query.width,
                                                 query.access,
                                                 query.maxHits,
                                                 query.timeoutMs,
                                                 query.module,
                                                 query.symbol}),
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
                     encodeDebugBreakpointRequest(
                         {query.pid, query.address, query.kind, query.timeoutMs, query.module, query.symbol}),
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

        const auto response = exchange(connection->fd(),
                                       MessageType::DebugStepRequest,
                                       encodeDebugStepRequest({query.vcpu, query.threadId}),
                                       error);
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

    std::optional<DebugAttachResponse> debugAttach(const std::filesystem::path&                           socketPath,
                                                   const DebugQuery&                                      query,
                                                   const std::function<void(const DebugAttachResponse&)>& whileAttached,
                                                   std::string&                                           error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto response =
            exchange(connection->fd(), MessageType::DebugAttachRequest, encodeDebugAttachRequest({}), error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::DebugAttachResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeDebugAttachResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed debug attach response";
            return std::nullopt;
        }
        if (whileAttached)
        {
            whileAttached(*decoded);
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

    std::optional<DebugBreakpointArmResponse> debugArmBreakpoint(const std::filesystem::path&   socketPath,
                                                                 const DebugBreakpointArmQuery& query,
                                                                 std::string&                   error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto response = exchange(connection->fd(),
                                       MessageType::DebugBreakpointArmRequest,
                                       encodeDebugBreakpointArmRequest({query.pid, query.address, query.kind}),
                                       error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::DebugBreakpointArmResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeDebugBreakpointArmResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed debug breakpoint arm response";
        }
        return decoded;
    }

    std::optional<DebugResourceRemoveResponse> debugRemoveBreakpoint(const std::filesystem::path&    socketPath,
                                                                     const DebugResourceRemoveQuery& query,
                                                                     std::string&                    error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto response = exchange(connection->fd(),
                                       MessageType::DebugBreakpointRemoveRequest,
                                       encodeDebugBreakpointRemoveRequest({query.id}),
                                       error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::DebugResourceRemoveResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeDebugResourceRemoveResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed debug resource remove response";
        }
        return decoded;
    }

    std::optional<std::vector<DebugBreakpointArmResponse>>
    debugListBreakpoints(const std::filesystem::path& socketPath, const DebugQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto response = exchange(
            connection->fd(), MessageType::DebugBreakpointListRequest, encodeDebugBreakpointListRequest({}), error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::DebugBreakpointListResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeDebugBreakpointListResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed debug breakpoint list response";
            return std::nullopt;
        }
        return decoded->breakpoints;
    }

    std::optional<DebugWatchArmResponse>
    debugArmWatch(const std::filesystem::path& socketPath, const DebugWatchArmQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto response =
            exchange(connection->fd(),
                     MessageType::DebugWatchArmRequest,
                     encodeDebugWatchArmRequest({query.pid, query.address, query.width, query.access}),
                     error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::DebugWatchArmResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeDebugWatchArmResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed debug watch arm response";
        }
        return decoded;
    }

    std::optional<DebugResourceRemoveResponse>
    debugRemoveWatch(const std::filesystem::path& socketPath, const DebugResourceRemoveQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto response = exchange(
            connection->fd(), MessageType::DebugWatchRemoveRequest, encodeDebugWatchRemoveRequest({query.id}), error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::DebugResourceRemoveResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeDebugResourceRemoveResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed debug resource remove response";
        }
        return decoded;
    }

    std::optional<std::vector<DebugWatchArmResponse>>
    debugListWatches(const std::filesystem::path& socketPath, const DebugQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto response =
            exchange(connection->fd(), MessageType::DebugWatchListRequest, encodeDebugWatchListRequest({}), error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::DebugWatchListResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeDebugWatchListResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed debug watch list response";
            return std::nullopt;
        }
        return decoded->watches;
    }

    std::optional<DebugStatus>
    debugContinueArmed(const std::filesystem::path& socketPath, const DebugQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto response =
            exchange(connection->fd(), MessageType::DebugContinueRequest, encodeDebugContinueRequest({}), error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::DebugContinueResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeDebugContinueResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed debug continue response";
        }
        return decoded;
    }

    std::optional<DebugWaitEnd> debugWait(const std::filesystem::path&                    socketPath,
                                          const DebugWaitQuery&                           query,
                                          const std::function<void(const DebugWaitHit&)>& onHit,
                                          std::string&                                    error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto requestId = detail::nextRequestId();
        if (!writeFrame(connection->fd(),
                        MessageType::DebugWaitRequest,
                        requestId,
                        encodeDebugWaitRequest({query.timeoutMs, query.maxHits}),
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
            if (header.type == MessageType::DebugWaitHit)
            {
                const auto decoded = decodeDebugWaitHit(payload);
                if (!decoded)
                {
                    error = "server returned malformed debug wait hit";
                    return std::nullopt;
                }
                if (onHit)
                {
                    onHit(*decoded);
                }
                continue;
            }
            if (header.type == MessageType::DebugWaitEnd)
            {
                const auto decoded = decodeDebugWaitEnd(payload);
                if (!decoded)
                {
                    error = "server returned a malformed debug wait end";
                    return std::nullopt;
                }
                return decoded;
            }
            error = "server returned an unexpected response";
            return std::nullopt;
        }
    }
} // namespace mpfshost
