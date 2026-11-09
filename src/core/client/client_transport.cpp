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
    FileDescriptor::FileDescriptor(int fd) : _fd {fd} {}

    FileDescriptor::~FileDescriptor()
    {
        if (_fd >= 0)
        {
            ::close(_fd);
        }
    }

    FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept : _fd {other._fd}
    {
        other._fd = -1;
    }

    FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept
    {
        if (this != &other)
        {
            if (_fd >= 0)
            {
                ::close(_fd);
            }
            _fd       = other._fd;
            other._fd = -1;
        }
        return *this;
    }

    int FileDescriptor::fd() const noexcept
    {
        return _fd;
    }

    int FileDescriptor::release() noexcept
    {
        const int descriptor = _fd;
        _fd                  = -1;
        return descriptor;
    }

    std::uint64_t nextRequestId()
    {
        static std::atomic<std::uint64_t> counter {1};
        return counter.fetch_add(1);
    }

    int connectToServer(const std::filesystem::path& socketPath, std::string& error)
    {
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
            return std::nullopt;
        }

        FrameHeader header;
        Response    response;
        const auto  status = readFrame(fd, header, response.payload, error);
        if (status != FrameReadStatus::Complete)
        {
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
            error = "server answered a different request id";
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
                error = "server answered a different request id";
                return std::nullopt;
            }
            if (header.type == MessageType::ErrorResponse)
            {
                if (const auto decoded = decodeErrorResponse(framePayload); decoded)
                {
                    error = decoded->message;
                }
                else
                {
                    error = "server returned a malformed error";
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
                error = decoded->message;
                if (errorCode != nullptr)
                {
                    *errorCode = decoded->code;
                }
            }
            else
            {
                error = "server returned a malformed error";
            }
            return std::nullopt;
        }
        if (response.type != expected)
        {
            error = "server returned an unexpected response";
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
            error = "server returned a malformed bind response";
            return std::nullopt;
        }
        return connection;
    }
} // namespace mpfshost::detail
