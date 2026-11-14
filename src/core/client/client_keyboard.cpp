#include "core/client/client_legacy.hpp"

#include "core/client/client_transport.hpp"

namespace mpfshost
{
    using detail::connectAndBind;
    using detail::exchange;
    using detail::expectResponse;

    std::optional<KeyboardStateResponse>
    keyboardState(const std::filesystem::path& socketPath, const KeyboardStateQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto response = exchange(
            connection->fd(), MessageType::KeyboardStateRequest, encodeKeyboardStateRequest({query.sessionId}), error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::KeyboardStateResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeKeyboardStateResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed keyboard state response";
        }
        return decoded;
    }
} // namespace mpfshost
