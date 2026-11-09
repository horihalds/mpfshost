#include "core/client/client.hpp"

#include "core/client/client_transport.hpp"

#include <string>

namespace mpfshost
{
    using detail::connectAndBind;
    using detail::exchange;
    using detail::expectResponse;

    std::optional<DisassembleResponse>
    disassemble(const std::filesystem::path& socketPath, const DisassembleQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto response = exchange(connection->fd(),
                                       MessageType::DisassembleRequest,
                                       encodeDisassembleRequest({query.pid, query.address, query.count}),
                                       error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::DisassembleResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeDisassembleResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed disassemble response";
        }
        return decoded;
    }
} // namespace mpfshost
