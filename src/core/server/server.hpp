#ifndef MPFSHOST_CORE_SERVER_SERVER_HPP
#define MPFSHOST_CORE_SERVER_SERVER_HPP
#pragma once

#include "core/server/access_policy.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mpfshost
{
    class TargetSource;
    class VmBackend;

    struct ServerOptions
    {
        ServerOptions() = default;

        explicit ServerOptions(std::filesystem::path path) : socketPath {std::move(path)} {}

        std::filesystem::path        socketPath;
        AccessPolicy                 access;
        std::uint32_t                socketMode {0666};
        std::optional<std::uint32_t> socketGid;
        std::size_t                  workers {8};
        std::size_t                  pendingConnections {64};
        std::chrono::milliseconds    requestTimeout {std::chrono::seconds {30}}; // zero disables
        std::chrono::milliseconds    idleTimeout {std::chrono::seconds {300}};   // zero disables
        bool                         allowVfsWrite {};
    };

    class Server
    {
    public:
        Server(ServerOptions options, const TargetSource& targets, VmBackend& backend);
        ~Server();

        Server(const Server&)            = delete;
        Server& operator=(const Server&) = delete;

        [[nodiscard]] bool run(std::string& error);
        void               requestStop();

        // Observability for the pool and the queue; the tests poll them instead of guessing timings.
        [[nodiscard]] std::size_t activeSessions() const noexcept;
        [[nodiscard]] std::size_t pendingConnections() const noexcept;

    private:
        struct PendingConnection
        {
            int                            fd {-1};
            std::uint64_t                  connectionId {};
            std::optional<PeerCredentials> credentials;
            AccessDecision                 decision {AccessDecision::Allowed};
        };

        void addClient(int fd);
        void removeClient(int fd);
        void applySocketTimeouts(int fd) const;
        void workerLoop();
        void refusalLoop();
        void serveConnection(const PendingConnection& connection);
        bool enqueueConnection(PendingConnection connection);
        void refuseConnection(PendingConnection connection);
        void drainPending();
        void drainRefusals();

        ServerOptions                 _options;
        const TargetSource&           _targets;
        VmBackend&                    _backend;
        std::atomic<bool>             _stopRequested {false};
        std::atomic<int>              _listenFd {-1};
        std::atomic<std::size_t>      _activeSessions {0};
        std::atomic<std::size_t>      _pendingCount {0};
        std::mutex                    _mutex;
        std::condition_variable       _queueCv;
        std::unordered_set<int>       _clients;
        std::deque<PendingConnection> _pending;
        std::vector<std::jthread>     _workers;
        bool                          _saturated {};
        std::mutex                    _refusalMutex;
        std::condition_variable       _refusalCv;
        std::deque<PendingConnection> _refusals;
        std::jthread                  _refusalWorker;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_SERVER_HPP
