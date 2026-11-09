#include "core/debug/qmp_client.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace mpfshost::debug
{
    namespace
    {
        using Json = nlohmann::json;

        std::chrono::milliseconds remaining(std::chrono::steady_clock::time_point deadline)
        {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            return left > std::chrono::milliseconds::zero() ? left : std::chrono::milliseconds::zero();
        }

        bool
        waitForDescriptor(int fd, short events, std::chrono::milliseconds timeout, bool& closed, std::string& error)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (true)
            {
                const auto left = remaining(deadline);
                if (left == std::chrono::milliseconds::zero())
                {
                    error = "the QMP socket timed out";
                    return false;
                }

                pollfd     descriptor {fd, events, 0};
                const auto result = ::poll(&descriptor, 1, static_cast<int>(left.count()));
                if (result == 0)
                {
                    error = "the QMP socket timed out";
                    return false;
                }
                if (result < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    error = std::string("poll on the QMP socket failed: ") + std::strerror(errno);
                    return false;
                }
                if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0)
                {
                    error = "the QMP socket reported an error";
                    return false;
                }
                if ((descriptor.revents & POLLHUP) != 0)
                {
                    closed = true;
                }
                return true;
            }
        }

        class UnixQmpTransport final : public QmpTransport
        {
        public:
            ~UnixQmpTransport() override
            {
                close();
            }

            [[nodiscard]] bool open(const std::filesystem::path& socketPath,
                                    std::chrono::milliseconds    timeout,
                                    std::string&                 error) override
            {
                close();

                const auto path = socketPath.string();
                if (path.empty())
                {
                    error = "the target has no QMP socket";
                    return false;
                }

                sockaddr_un address {};
                if (path.size() >= sizeof(address.sun_path))
                {
                    error = "the QMP socket path is too long";
                    return false;
                }

                const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
                if (fd < 0)
                {
                    error = std::string("socket for the QMP channel failed: ") + std::strerror(errno);
                    return false;
                }

                address.sun_family = AF_UNIX;
                std::memcpy(address.sun_path, path.c_str(), path.size() + 1);

                if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
                {
                    if (errno != EINPROGRESS)
                    {
                        error = "connect to the QMP socket failed: " + std::string(std::strerror(errno));
                        ::close(fd);
                        return false;
                    }
                    bool closed = false;
                    if (!waitForDescriptor(fd, POLLOUT, timeout, closed, error))
                    {
                        if (error.empty())
                        {
                            error = "the QMP socket closed while connecting";
                        }
                        ::close(fd);
                        return false;
                    }
                    int       socketError = 0;
                    socklen_t length      = sizeof(socketError);
                    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &length) != 0 || socketError != 0)
                    {
                        error = "connect to the QMP socket failed: " + std::string(std::strerror(socketError));
                        ::close(fd);
                        return false;
                    }
                }

                _fd = fd;
                _buffer.clear();
                return true;
            }

            [[nodiscard]] bool
            writeLine(std::string_view line, std::chrono::milliseconds timeout, std::string& error) override
            {
                if (_fd < 0)
                {
                    error = "the QMP channel is not open";
                    return false;
                }

                std::string message {line};
                message.push_back('\n');
                const auto  deadline = std::chrono::steady_clock::now() + timeout;
                std::size_t offset   = 0;
                while (offset < message.size())
                {
                    const auto left = remaining(deadline);
                    if (left == std::chrono::milliseconds::zero())
                    {
                        error = "the QMP socket timed out while writing";
                        return false;
                    }
                    bool closed = false;
                    if (!waitForDescriptor(_fd, POLLOUT, left, closed, error))
                    {
                        return false;
                    }
                    if (closed)
                    {
                        error = "the QMP connection closed while writing";
                        return false;
                    }

                    const auto written = ::send(_fd, message.data() + offset, message.size() - offset, MSG_NOSIGNAL);
                    if (written < 0)
                    {
                        if (errno == EINTR || errno == EAGAIN)
                        {
                            continue;
                        }
                        error = std::string("writing to the QMP socket failed: ") + std::strerror(errno);
                        return false;
                    }
                    offset += static_cast<std::size_t>(written);
                }
                return true;
            }

            [[nodiscard]] bool
            readLine(std::string& line, std::chrono::milliseconds timeout, bool& closed, std::string& error) override
            {
                if (_fd < 0)
                {
                    error = "the QMP channel is not open";
                    return false;
                }

                const auto deadline = std::chrono::steady_clock::now() + timeout;
                while (true)
                {
                    const auto newline = _buffer.find('\n');
                    if (newline != std::string::npos)
                    {
                        line = _buffer.substr(0, newline);
                        _buffer.erase(0, newline + 1);
                        if (!line.empty() && line.back() == '\r')
                        {
                            line.pop_back();
                        }
                        return true;
                    }
                    if (_buffer.size() > kMaxQmpLineLength)
                    {
                        error = "the QMP peer sent a line that is too long";
                        return false;
                    }

                    const auto left = remaining(deadline);
                    if (left == std::chrono::milliseconds::zero())
                    {
                        error = "the QMP socket timed out";
                        return false;
                    }
                    bool hungUp = false;
                    if (!waitForDescriptor(_fd, POLLIN, left, hungUp, error))
                    {
                        return false;
                    }

                    char       chunk[4096];
                    const auto received = ::recv(_fd, chunk, sizeof(chunk), 0);
                    if (received > 0)
                    {
                        _buffer.append(chunk, static_cast<std::size_t>(received));
                        continue;
                    }
                    if (received == 0)
                    {
                        closed = true;
                        error  = "the QMP connection closed";
                        return false;
                    }
                    if (errno == EINTR || errno == EAGAIN)
                    {
                        continue;
                    }
                    error = std::string("reading from the QMP socket failed: ") + std::strerror(errno);
                    return false;
                }
            }

            void close() noexcept override
            {
                if (_fd >= 0)
                {
                    ::close(_fd);
                    _fd = -1;
                }
                _buffer.clear();
            }

        private:
            int         _fd {-1};
            std::string _buffer;
        };

        std::string qmpErrorText(const Json& message)
        {
            const auto& error = message.at("error");
            if (!error.is_object())
            {
                return "the QMP peer returned an error";
            }
            const auto errorClass  = error.contains("class") && error.at("class").is_string()
                                       ? error.at("class").get<std::string>()
                                       : std::string {"unknown"};
            const auto description = error.contains("desc") && error.at("desc").is_string()
                                       ? error.at("desc").get<std::string>()
                                       : std::string {};
            return description.empty() ? "QMP error " + errorClass : "QMP error " + errorClass + ": " + description;
        }
    } // namespace

    std::unique_ptr<QmpTransport> makeUnixQmpTransport()
    {
        return std::make_unique<UnixQmpTransport>();
    }

    QmpClient::QmpClient(std::unique_ptr<QmpTransport> transport) : _transport {std::move(transport)} {}

    QmpClient::~QmpClient()
    {
        if (_open && _transport)
        {
            _transport->close();
        }
    }

    bool
    QmpClient::connect(const std::filesystem::path& socketPath, std::chrono::milliseconds timeout, std::string& error)
    {
        if (!_transport)
        {
            error = "the QMP client has no transport";
            return false;
        }
        if (!_transport->open(socketPath, timeout, error))
        {
            return false;
        }
        _open = true;

        std::string line;
        bool        closed = false;
        if (!_transport->readLine(line, timeout, closed, error))
        {
            if (closed)
            {
                _open = false;
            }
            if (error.empty())
            {
                error = "the QMP peer did not send a greeting";
            }
            return false;
        }

        Json greeting;
        try
        {
            greeting = Json::parse(line);
        }
        catch (const Json::exception&)
        {
            error = "the QMP greeting is not valid JSON";
            return false;
        }
        if (!greeting.is_object() || !greeting.contains("QMP"))
        {
            error = "the QMP peer did not send a greeting";
            return false;
        }

        std::string ignored;
        return execute("qmp_capabilities", ignored, timeout, error);
    }

    bool QmpClient::execute(std::string_view          command,
                            std::string&              resultJson,
                            std::chrono::milliseconds timeout,
                            std::string&              error)
    {
        const auto message = Json {
            {"execute",
             std::string {
                 command}}
        }.dump();
        return executeMessage(message, resultJson, timeout, error);
    }

    bool QmpClient::execute(std::string_view          command,
                            std::string_view          argumentsJson,
                            std::string&              resultJson,
                            std::chrono::milliseconds timeout,
                            std::string&              error)
    {
        Json message {
            {"execute", std::string {command}}
        };
        if (!argumentsJson.empty())
        {
            try
            {
                message["arguments"] = Json::parse(argumentsJson);
            }
            catch (const Json::exception&)
            {
                error = "the QMP arguments are not valid JSON";
                return false;
            }
        }
        return executeMessage(message.dump(), resultJson, timeout, error);
    }

    bool QmpClient::executeMessage(std::string_view          messageJson,
                                   std::string&              resultJson,
                                   std::chrono::milliseconds timeout,
                                   std::string&              error)
    {
        if (!_open)
        {
            error = "the QMP channel is not open";
            return false;
        }

        if (!_transport->writeLine(messageJson, timeout, error))
        {
            // A partial write leaves the command stream unusable, so the next call must reconnect.
            _open = false;
            return false;
        }
        return waitForReply(resultJson, timeout, error);
    }

    bool QmpClient::humanMonitor(std::string_view          commandLine,
                                 std::string&              output,
                                 std::chrono::milliseconds timeout,
                                 std::string&              error)
    {
        const auto message =
            Json {
                {  "execute",                       "human-monitor-command"},
                {"arguments", {{"command-line", std::string {commandLine}}}}
        }
                .dump();

        std::string resultJson;
        if (!executeMessage(message, resultJson, timeout, error))
        {
            return false;
        }

        Json result;
        try
        {
            result = Json::parse(resultJson);
        }
        catch (const Json::exception&)
        {
            error = "the HMP reply is not valid JSON";
            return false;
        }
        if (!result.is_string())
        {
            error = "the HMP reply is not a string";
            return false;
        }
        output = result.get<std::string>();
        return true;
    }

    bool QmpClient::waitForReply(std::string& resultJson, std::chrono::milliseconds timeout, std::string& error)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true)
        {
            const auto left = remaining(deadline);
            if (left == std::chrono::milliseconds::zero())
            {
                error = "timed out waiting for a QMP reply";
                return false;
            }

            std::string line;
            bool        closed = false;
            if (!_transport->readLine(line, left, closed, error))
            {
                // The reply is gone or arrived too late; keeping the channel would desynchronize the next
                // command because QMP replies carry no request id.
                _open = false;
                if (error.empty())
                {
                    error = closed ? "the QMP connection closed" : "timed out waiting for a QMP reply";
                }
                return false;
            }

            Json message;
            try
            {
                message = Json::parse(line);
            }
            catch (const Json::exception&)
            {
                _open = false;
                error = "the QMP peer sent a malformed JSON line";
                return false;
            }
            if (!message.is_object())
            {
                _open = false;
                error = "the QMP peer sent a JSON value that is not an object";
                return false;
            }
            if (message.contains("event"))
            {
                continue;
            }
            if (message.contains("error"))
            {
                error = qmpErrorText(message);
                return false;
            }
            if (!message.contains("return"))
            {
                _open = false;
                error = "the QMP peer sent a message with neither return nor error";
                return false;
            }
            resultJson = message.at("return").dump();
            return true;
        }
    }

    bool QmpClient::queryStatus(QmpStatusInfo& info, std::chrono::milliseconds timeout, std::string& error)
    {
        std::string resultJson;
        if (!execute("query-status", resultJson, timeout, error))
        {
            return false;
        }

        Json result;
        try
        {
            result = Json::parse(resultJson);
        }
        catch (const Json::exception&)
        {
            error = "the QMP query-status reply is not valid JSON";
            return false;
        }
        if (!result.is_object() || !result.contains("status") || !result.at("status").is_string())
        {
            error = "the QMP query-status reply has no status";
            return false;
        }

        info.status     = result.at("status").get<std::string>();
        info.running    = info.status == "running";
        info.singlestep = false;
        if (result.contains("running") && result.at("running").is_boolean())
        {
            info.running = result.at("running").get<bool>();
        }
        if (result.contains("singlestep") && result.at("singlestep").is_boolean())
        {
            info.singlestep = result.at("singlestep").get<bool>();
        }
        return true;
    }

    bool QmpClient::queryVcpuCount(std::uint32_t& count, std::chrono::milliseconds timeout, std::string& error)
    {
        std::string resultJson;
        if (!execute("query-cpus-fast", resultJson, timeout, error))
        {
            return false;
        }

        Json result;
        try
        {
            result = Json::parse(resultJson);
        }
        catch (const Json::exception&)
        {
            error = "the QMP query-cpus-fast reply is not valid JSON";
            return false;
        }
        if (!result.is_array())
        {
            error = "the QMP query-cpus-fast reply is not a list";
            return false;
        }
        count = static_cast<std::uint32_t>(result.size());
        return true;
    }

    DebugRunState qmpRunState(std::string_view status) noexcept
    {
        if (status == "running")
        {
            return DebugRunState::Running;
        }
        if (status == "paused")
        {
            return DebugRunState::Paused;
        }
        if (status == "shutdown")
        {
            return DebugRunState::Shutdown;
        }
        if (status == "suspended")
        {
            return DebugRunState::Suspended;
        }
        if (status == "guest-panicked")
        {
            return DebugRunState::GuestPanicked;
        }
        if (status.empty())
        {
            return DebugRunState::Unknown;
        }
        return DebugRunState::Other;
    }
} // namespace mpfshost::debug
