#include "core/server/server.hpp"

#include "core/ipc/message.hpp"
#include "core/log/log.hpp"
#include "core/server/access_policy.hpp"
#include "core/server/backend.hpp"
#include "core/server/session.hpp"
#include "core/util/identity.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace mpfshost
{
    namespace
    {
        std::uint64_t nextConnectionId()
        {
            static std::atomic<std::uint64_t> counter {1};
            return counter.fetch_add(1);
        }

        std::string formatMode(std::uint32_t mode)
        {
            char buffer[8] {};
            std::snprintf(buffer, sizeof(buffer), "%04o", static_cast<unsigned>(mode));
            return buffer;
        }

        void logPeer(const std::optional<PeerCredentials>& credentials, std::uint64_t connectionId)
        {
            if (!credentials)
            {
                log::warn("server",
                          "client credentials unavailable, allowing because the access policy is open conn=",
                          connectionId);
                return;
            }
            log::info("server",
                      "client connected uid=",
                      credentials->uid,
                      " gid=",
                      credentials->gid,
                      " pid=",
                      credentials->pid,
                      " conn=",
                      connectionId);
        }

        std::string denialMessage(const std::optional<PeerCredentials>& credentials, const std::string& policy)
        {
            if (!credentials)
            {
                return "access denied: the host could not read your peer credentials";
            }
            return "access denied: uid " + std::to_string(credentials->uid) + " gid " + std::to_string(credentials->gid)
                 + " is not allowed by the host access policy (" + policy + ')';
        }

        timeval socketTimeout(std::chrono::milliseconds timeout)
        {
            timeval value {};
            value.tv_sec  = static_cast<time_t>(timeout.count() / 1000);
            value.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
            return value;
        }

        constexpr std::size_t      kRefusalQueueLimit = 16;
        constexpr std::string_view kBusyMessage       = "host is busy: the connection queue is full, retry shortly";

        // Waits for the peer's first request so the refusal can echo its request id; the client rejects
        // responses carrying any other id, and an unsolicited write would race the peer's own request.
        void refuseAfterFirstFrame(int                       fd,
                                   std::uint64_t             connectionId,
                                   ErrorCode                 code,
                                   const std::string&        message,
                                   std::chrono::milliseconds timeout)
        {
            const auto value = socketTimeout(timeout);
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));

            FrameHeader            header;
            std::vector<std::byte> payload;
            std::string            error;
            if (readFrame(fd, header, payload, error) != FrameReadStatus::Complete)
            {
                log::debug("server", "could not read the request to refuse conn=", connectionId, ": ", error);
                return;
            }

            const auto response = encodeErrorResponse({code, message});
            if (!writeFrame(fd, MessageType::ErrorResponse, header.requestId, response, error))
            {
                log::debug("server", "could not deliver the refusal conn=", connectionId, ": ", error);
            }
        }

        void rejectPeer(int                                   fd,
                        std::uint64_t                         connectionId,
                        const std::optional<PeerCredentials>& credentials,
                        const std::string&                    policy)
        {
            const auto reason = denialMessage(credentials, policy);
            if (credentials)
            {
                log::warn("server",
                          "client refused uid=",
                          credentials->uid,
                          " gid=",
                          credentials->gid,
                          " pid=",
                          credentials->pid,
                          " conn=",
                          connectionId,
                          ": ",
                          reason);
            }
            else
            {
                log::warn("server", "client refused conn=", connectionId, ": ", reason);
            }

            refuseAfterFirstFrame(
                fd, connectionId, ErrorCode::AccessDenied, reason, std::chrono::seconds {kAccessRejectTimeoutSeconds});
        }
    } // namespace

    Server::Server(ServerOptions options, const TargetSource& targets, VmBackend& backend)
        : _options {std::move(options)}, _targets {targets}, _backend {backend}
    {
    }

    Server::~Server()
    {
        requestStop();

        std::vector<std::jthread> workers;
        {
            std::lock_guard lock {_mutex};
            workers = std::move(_workers);
        }
        for (auto& worker : workers)
        {
            worker.join();
        }
        if (_refusalWorker.joinable())
        {
            _refusalWorker.join();
        }
        drainPending();
        drainRefusals();

        std::error_code error;
        std::filesystem::remove(_options.socketPath, error);
    }

    bool Server::run(std::string& error)
    {
        error.clear();
        if (_listenFd.load() >= 0)
        {
            error = "server is already running";
            return false;
        }

        std::error_code filesystemError;
        std::filesystem::remove(_options.socketPath, filesystemError);

        const auto path = _options.socketPath.string();
        if (path.size() >= sizeof(sockaddr_un::sun_path))
        {
            error = "socket path is too long";
            log::error("server", error);
            return false;
        }

        const int listenFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listenFd < 0)
        {
            error = std::string("socket failed: ") + std::strerror(errno);
            log::error("server", error);
            return false;
        }

        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);

        if (::bind(listenFd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
        {
            error = std::string("bind failed: ") + std::strerror(errno);
            log::error("server", error);
            ::close(listenFd);
            return false;
        }

        if (_options.socketGid
            && ::chown(path.c_str(), static_cast<uid_t>(-1), static_cast<gid_t>(*_options.socketGid)) != 0)
        {
            error = std::string("chown failed: ") + std::strerror(errno);
            log::error("server", error);
            ::close(listenFd);
            std::filesystem::remove(_options.socketPath, filesystemError);
            return false;
        }

        if (::chmod(path.c_str(), static_cast<mode_t>(_options.socketMode)) != 0)
        {
            error = std::string("chmod failed: ") + std::strerror(errno);
            log::error("server", error);
            ::close(listenFd);
            std::filesystem::remove(_options.socketPath, filesystemError);
            return false;
        }

        if (::listen(listenFd, SOMAXCONN) != 0)
        {
            error = std::string("listen failed: ") + std::strerror(errno);
            log::error("server", error);
            ::close(listenFd);
            std::filesystem::remove(_options.socketPath, filesystemError);
            return false;
        }

        _listenFd.store(listenFd);
        std::string listening = "listening on " + path + " (mode " + formatMode(_options.socketMode);
        if (_options.socketGid)
        {
            const auto group = readGroupInfo(*_options.socketGid);
            listening += ", group ";
            if (group && !group->name.empty())
            {
                listening += '\'' + group->name + "' gid " + std::to_string(*_options.socketGid);
            }
            else
            {
                listening += std::to_string(*_options.socketGid);
            }
        }
        listening += ')';
        log::info("server", listening);

        const auto workerCount = std::max<std::size_t>(1, _options.workers);
        log::info("server",
                  "limits: ",
                  workerCount,
                  " worker(s), ",
                  _options.pendingConnections,
                  " pending connection(s), request timeout ",
                  _options.requestTimeout.count(),
                  " ms, idle timeout ",
                  _options.idleTimeout.count(),
                  " ms (0 disables)");
        {
            std::lock_guard lock {_mutex};
            _workers.reserve(workerCount);
            for (std::size_t index = 0; index < workerCount; ++index)
            {
                _workers.emplace_back(
                    [this]
                    {
                        workerLoop();
                    });
            }
        }
        _refusalWorker = std::jthread {[this]
                                       {
                                           refusalLoop();
                                       }};

        if (_options.access.enforcing())
        {
            log::info("server", "access policy: ", _options.access.describe());
        }
        else
        {
            log::warn("server",
                      "access control is disabled by --allow-all: every local user may connect; pass --allow-uid, "
                      "--allow-gid, --allow-user or --allow-group to restrict clients");
        }

        while (!_stopRequested.load())
        {
            const int clientFd = ::accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
            if (clientFd < 0)
            {
                if (errno == EINTR && !_stopRequested.load())
                {
                    continue;
                }
                break;
            }

            if (_stopRequested.load())
            {
                ::close(clientFd);
                break;
            }

            applySocketTimeouts(clientFd);

            const auto connectionId = nextConnectionId();
            const auto credentials  = readPeerCredentials(clientFd);
            const auto decision     = _options.access.decide(credentials);
            if (decision == AccessDecision::Allowed)
            {
                logPeer(credentials, connectionId);
            }
            addClient(clientFd);

            if (enqueueConnection(PendingConnection {clientFd, connectionId, credentials, decision}))
            {
                continue;
            }

            log::debug("server", "connection queue is full, refusing conn=", connectionId);
            refuseConnection(PendingConnection {clientFd, connectionId, credentials, decision});
        }

        if (_listenFd.exchange(-1) >= 0)
        {
            ::close(listenFd);
        }
        std::filesystem::remove(_options.socketPath, filesystemError);
        return true;
    }

    void Server::requestStop()
    {
        _stopRequested.store(true);
        _queueCv.notify_all();
        _refusalCv.notify_all();

        const int listenFd = _listenFd.exchange(-1);
        if (listenFd >= 0)
        {
            ::shutdown(listenFd, SHUT_RDWR);
            ::close(listenFd);
        }

        std::lock_guard lock {_mutex};
        for (const int clientFd : _clients)
        {
            ::shutdown(clientFd, SHUT_RDWR);
        }
    }

    std::size_t Server::activeSessions() const noexcept
    {
        return _activeSessions.load();
    }

    std::size_t Server::pendingConnections() const noexcept
    {
        return _pendingCount.load();
    }

    bool Server::enqueueConnection(PendingConnection connection)
    {
        bool accepted        = false;
        bool enterSaturation = false;
        {
            std::lock_guard lock {_mutex};
            if (_stopRequested.load() || _pending.size() >= _options.pendingConnections)
            {
                if (!_stopRequested.load() && !_saturated)
                {
                    _saturated      = true;
                    enterSaturation = true;
                }
            }
            else
            {
                _pending.push_back(std::move(connection));
                _pendingCount.store(_pending.size());
                _saturated = false;
                accepted   = true;
            }
        }

        if (enterSaturation)
        {
            log::warn("server", "the connection queue is full; refusing new clients until a worker frees a slot");
        }
        if (accepted)
        {
            _queueCv.notify_one();
        }
        return accepted;
    }

    void Server::refuseConnection(PendingConnection connection)
    {
        bool queued = false;
        {
            std::lock_guard lock {_refusalMutex};
            if (!_stopRequested.load() && _refusals.size() < kRefusalQueueLimit)
            {
                _refusals.push_back(std::move(connection));
                queued = true;
            }
        }

        if (queued)
        {
            _refusalCv.notify_one();
            return;
        }

        // The refusal queue itself is full: close without echoing, since there is nowhere to wait.
        log::warn(
            "server", "the refusal queue is full, dropping conn=", connection.connectionId, " without a busy response");
        ::shutdown(connection.fd, SHUT_RDWR);
        removeClient(connection.fd);
        ::close(connection.fd);
        log::info("server", "client disconnected conn=", connection.connectionId);
    }

    void Server::refusalLoop()
    {
        while (true)
        {
            PendingConnection connection;
            {
                std::unique_lock lock {_refusalMutex};
                _refusalCv.wait(lock,
                                [this]
                                {
                                    return _stopRequested.load() || !_refusals.empty();
                                });
                if (_stopRequested.load())
                {
                    return;
                }
                connection = std::move(_refusals.front());
                _refusals.pop_front();
            }

            // The refusal thread owns the wait; the accept loop stays free for new clients.
            refuseAfterFirstFrame(connection.fd,
                                  connection.connectionId,
                                  ErrorCode::Busy,
                                  std::string {kBusyMessage},
                                  std::chrono::seconds {1});
            ::shutdown(connection.fd, SHUT_RDWR);
            removeClient(connection.fd);
            ::close(connection.fd);
            log::info("server", "client disconnected conn=", connection.connectionId);
        }
    }

    void Server::workerLoop()
    {
        while (true)
        {
            PendingConnection connection;
            {
                std::unique_lock lock {_mutex};
                _queueCv.wait(lock,
                              [this]
                              {
                                  return _stopRequested.load() || !_pending.empty();
                              });
                if (_stopRequested.load())
                {
                    return;
                }
                connection = std::move(_pending.front());
                _pending.pop_front();
                _pendingCount.store(_pending.size());
            }

            _activeSessions.fetch_add(1);
            serveConnection(connection);
            _activeSessions.fetch_sub(1);
        }
    }

    void Server::serveConnection(const PendingConnection& connection)
    {
        if (connection.decision == AccessDecision::Allowed)
        {
            Session session {connection.fd,
                             connection.connectionId,
                             _targets,
                             _backend,
                             _options.requestTimeout,
                             _options.allowVfsWrite};
            session.run();
            removeClient(connection.fd);
            log::info("server", "client disconnected conn=", connection.connectionId);
            return;
        }

        rejectPeer(connection.fd, connection.connectionId, connection.credentials, _options.access.describe());
        ::shutdown(connection.fd, SHUT_RDWR);
        removeClient(connection.fd);
        ::close(connection.fd);
        log::info("server", "client disconnected conn=", connection.connectionId);
    }

    void Server::drainPending()
    {
        std::deque<PendingConnection> pending;
        {
            std::lock_guard lock {_mutex};
            pending.swap(_pending);
            _pendingCount.store(0);
        }
        for (auto& connection : pending)
        {
            ::shutdown(connection.fd, SHUT_RDWR);
            removeClient(connection.fd);
            ::close(connection.fd);
        }
    }

    void Server::drainRefusals()
    {
        std::deque<PendingConnection> refusals;
        {
            std::lock_guard lock {_refusalMutex};
            refusals.swap(_refusals);
        }
        for (auto& connection : refusals)
        {
            ::shutdown(connection.fd, SHUT_RDWR);
            removeClient(connection.fd);
            ::close(connection.fd);
        }
    }

    void Server::addClient(int fd)
    {
        std::lock_guard lock {_mutex};
        _clients.insert(fd);
    }

    void Server::removeClient(int fd)
    {
        std::lock_guard lock {_mutex};
        _clients.erase(fd);
    }

    void Server::applySocketTimeouts(int fd) const
    {
        if (_options.idleTimeout.count() > 0)
        {
            const auto timeout = socketTimeout(_options.idleTimeout);
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        }
        if (_options.requestTimeout.count() > 0)
        {
            const auto timeout = socketTimeout(_options.requestTimeout);
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        }
    }
} // namespace mpfshost
