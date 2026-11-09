#include "core/client/client_transport.hpp"

#include "core/client/connect_diagnostics.hpp"
#include "core/ipc/message.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace mpfshost::detail
{
    namespace
    {
        thread_local ActiveConnection* t_activeConnection = nullptr;
        thread_local ErrorCode         t_lastErrorCode    = ErrorCode::Internal;
    } // namespace

    FileDescriptor::FileDescriptor(int fd, bool owns) : _fd {fd}, _owns {owns} {}

    FileDescriptor::~FileDescriptor()
    {
        if (_fd >= 0 && _owns)
        {
            ::close(_fd);
        }
    }

    FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept : _fd {other._fd}, _owns {other._owns}
    {
        other._fd   = -1;
        other._owns = false;
    }

    FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept
    {
        if (this != &other)
        {
            if (_fd >= 0 && _owns)
            {
                ::close(_fd);
            }
            _fd         = other._fd;
            _owns       = other._owns;
            other._fd   = -1;
            other._owns = false;
        }
        return *this;
    }

    int FileDescriptor::fd() const noexcept
    {
        return _fd;
    }

    bool FileDescriptor::owns() const noexcept
    {
        return _owns;
    }

    int FileDescriptor::release() noexcept
    {
        const int descriptor = _fd;
        _fd                  = -1;
        _owns                = false;
        return descriptor;
    }

    ActiveConnectionScope::ActiveConnectionScope(int fd, std::string targetName) : _previous {t_activeConnection}
    {
        static thread_local ActiveConnection current;
        current            = {fd, std::move(targetName)};
        t_activeConnection = &current;
    }

    ActiveConnectionScope::~ActiveConnectionScope()
    {
        t_activeConnection = _previous;
    }

    ActiveConnection* activeConnection() noexcept
    {
        return t_activeConnection;
    }

    ErrorCode lastErrorCode() noexcept
    {
        return t_lastErrorCode;
    }

    void resetLastErrorCode() noexcept
    {
        t_lastErrorCode = ErrorCode::Internal;
    }

    std::uint64_t nextRequestId()
    {
        static std::atomic<std::uint64_t> counter {1};
        return counter.fetch_add(1);
    }

    int connectToServer(const std::filesystem::path& socketPath, std::string& error)
    {
        t_lastErrorCode = ErrorCode::ConnectionFailed;
        const auto text = socketPath.string();
        if (text.size() >= sizeof(sockaddr_un::sun_path))
        {
            error = "socket path is too long";
            return -1;
        }

        for (int attempt = 0; attempt < 100; ++attempt)
        {
            const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd < 0)
            {
                error = std::string("socket failed: ") + std::strerror(errno);
                return -1;
            }

            sockaddr_un address {};
            address.sun_family = AF_UNIX;
            std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
            if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0)
            {
                return fd;
            }

            const int connectError = errno;
            ::close(fd);
            if (connectError != ENOENT && connectError != ECONNREFUSED)
            {
                error = describeConnectDenial(collectConnectDenialContext(socketPath, std::strerror(connectError)));
                return -1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        error = "could not connect to '" + text + "': ";
        if (readSocketFileInfo(socketPath))
        {
            error += "the socket file is present but nothing is listening (is mpfshost running?)";
        }
        else
        {
            error += "the socket file is missing";
        }
        return -1;
    }

    std::optional<Response> exchange(int fd, MessageType type, std::span<const std::byte> payload, std::string& error)
    {
        const auto requestId = nextRequestId();
        if (!writeFrame(fd, type, requestId, payload, error))
        {
            t_lastErrorCode = ErrorCode::ConnectionFailed;
            return std::nullopt;
        }

        FrameHeader header;
        Response    response;
        const auto  status = readFrame(fd, header, response.payload, error);
        if (status != FrameReadStatus::Complete)
        {
            t_lastErrorCode = ErrorCode::ConnectionFailed;
            if (status == FrameReadStatus::Timeout)
            {
                error = "the host timed out waiting for this request";
            }
            else if (error.empty())
            {
                error = "server closed the connection";
            }
            return std::nullopt;
        }
        if (header.requestId != requestId)
        {
            t_lastErrorCode = ErrorCode::ProtocolError;
            error           = "server answered a different request id";
            return std::nullopt;
        }

        response.type = header.type;
        return response;
    }

    std::optional<StreamedResponse> exchangeStreamed(
        int fd, MessageType type, std::span<const std::byte> payload, MessageType endType, std::string& error)
    {
        const auto requestId = nextRequestId();
        if (!writeFrame(fd, type, requestId, payload, error))
        {
            t_lastErrorCode = ErrorCode::ConnectionFailed;
            return std::nullopt;
        }

        StreamedResponse result;
        while (true)
        {
            FrameHeader            header;
            std::vector<std::byte> framePayload;
            const auto             status = readFrame(fd, header, framePayload, error);
            if (status != FrameReadStatus::Complete)
            {
                t_lastErrorCode = ErrorCode::ConnectionFailed;
                if (!result.data.empty())
                {
                    error = "the host abandoned the response: request deadline exceeded or the request was "
                            "canceled";
                }
                else if (status == FrameReadStatus::Timeout)
                {
                    error = "the host timed out waiting for this request";
                }
                else if (error.empty())
                {
                    error = "server closed the connection";
                }
                return std::nullopt;
            }
            if (header.requestId != requestId)
            {
                t_lastErrorCode = ErrorCode::ProtocolError;
                error           = "server answered a different request id";
                return std::nullopt;
            }
            if (header.type == MessageType::ErrorResponse)
            {
                if (const auto decoded = decodeErrorResponse(framePayload); decoded)
                {
                    t_lastErrorCode = decoded->code;
                    error           = decoded->message;
                }
                else
                {
                    t_lastErrorCode = ErrorCode::ProtocolError;
                    error           = "server returned a malformed error";
                }
                return std::nullopt;
            }
            if (header.type == endType)
            {
                result.endPayload = std::move(framePayload);
                return result;
            }
            result.data.push_back(std::move(framePayload));
        }
    }

    std::optional<std::vector<std::byte>>
    expectResponse(const Response& response, MessageType expected, std::string& error, ErrorCode* errorCode)
    {
        if (errorCode != nullptr)
        {
            *errorCode = ErrorCode::Internal;
        }
        if (response.type == MessageType::ErrorResponse)
        {
            if (const auto decoded = decodeErrorResponse(response.payload); decoded)
            {
                t_lastErrorCode = decoded->code;
                error           = decoded->message;
                if (errorCode != nullptr)
                {
                    *errorCode = decoded->code;
                }
            }
            else
            {
                t_lastErrorCode = ErrorCode::ProtocolError;
                error           = "server returned a malformed error";
            }
            return std::nullopt;
        }
        if (response.type != expected)
        {
            t_lastErrorCode = ErrorCode::ProtocolError;
            error           = "server returned an unexpected response";
            return std::nullopt;
        }
        return response.payload;
    }

    std::optional<Response> request(const std::filesystem::path& socketPath,
                                    MessageType                  type,
                                    std::span<const std::byte>   payload,
                                    std::string&                 error)
    {
        error.clear();
        if (const auto* active = activeConnection(); active != nullptr)
        {
            return exchange(active->fd, type, payload, error);
        }

        const int fd = connectToServer(socketPath, error);
        if (fd < 0)
        {
            return std::nullopt;
        }
        const FileDescriptor connection {fd};
        return exchange(fd, type, payload, error);
    }

    std::optional<FileDescriptor>
    connectAndBind(const std::filesystem::path& socketPath, const std::string& targetName, std::string& error)
    {
        error.clear();
        if (const auto* active = activeConnection(); active != nullptr)
        {
            if (!active->targetName.empty() && active->targetName != targetName)
            {
                t_lastErrorCode = ErrorCode::InvalidArgument;
                error           = "the active session is bound to a different target";
                return std::nullopt;
            }
            return FileDescriptor {active->fd, false};
        }

        const int fd = connectToServer(socketPath, error);
        if (fd < 0)
        {
            return std::nullopt;
        }
        FileDescriptor connection {fd};

        const auto bindPayload  = encodeBindRequest({targetName});
        const auto bindResponse = exchange(fd, MessageType::BindRequest, bindPayload, error);
        if (!bindResponse)
        {
            return std::nullopt;
        }

        const auto boundPayload = expectResponse(*bindResponse, MessageType::BindResponse, error);
        if (!boundPayload)
        {
            return std::nullopt;
        }
        if (!decodeBindResponse(*boundPayload))
        {
            t_lastErrorCode = ErrorCode::ProtocolError;
            error           = "server returned a malformed bind response";
            return std::nullopt;
        }
        return connection;
    }
} // namespace mpfshost::detail
