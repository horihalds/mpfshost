#ifndef MPFSHOST_CORE_SERVER_SCATTER_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_SCATTER_PROVIDER_HPP

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mpfshost
{
    struct ScatterRequestEntry
    {
        std::uint64_t          address {};
        std::uint32_t          size {};
        bool                   write {};
        std::vector<std::byte> data;
    };

    struct ScatterResult
    {
        std::uint64_t          address {};
        std::uint32_t          requestedSize {};
        std::uint32_t          bytesRead {};
        bool                   ok {};
        std::vector<std::byte> data;
    };

    class ScatterHandle
    {
    public:
        virtual ~ScatterHandle() = default;

        [[nodiscard]] virtual bool prepare(const std::vector<ScatterRequestEntry>& entries, std::string& error) = 0;
        [[nodiscard]] virtual bool execute(std::vector<ScatterResult>& results, std::string& error)             = 0;
        [[nodiscard]] virtual bool executeRead(std::vector<ScatterResult>& results, std::string& error)         = 0;
        [[nodiscard]] virtual bool clear(std::string& error)                                                    = 0;
    };

    class ScatterProvider
    {
    public:
        virtual ~ScatterProvider() = default;

        [[nodiscard]] virtual std::shared_ptr<ScatterHandle> createScatter(std::uint32_t pid, std::string& error) = 0;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_SCATTER_PROVIDER_HPP
