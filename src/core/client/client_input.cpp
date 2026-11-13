#include "core/client/client_legacy.hpp"

#include "core/client/client_transport.hpp"

namespace mpfshost
{
    using detail::connectAndBind;
    using detail::exchange;
    using detail::expectResponse;

    std::optional<InputEventResponse> sendInput(const std::filesystem::path&   socketPath,
                                                const DebugQuery&              query,
                                                const std::vector<InputEvent>& events,
                                                std::string&                   error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto response =
            exchange(connection->fd(), MessageType::InputEventRequest, encodeInputEventRequest({events}), error);
        if (!response)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*response, MessageType::InputEventResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeInputEventResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed input response";
        }
        return decoded;
    }
} // namespace mpfshost
