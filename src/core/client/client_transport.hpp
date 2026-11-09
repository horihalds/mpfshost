#ifndef MPFSHOST_CORE_CLIENT_CLIENT_TRANSPORT_HPP
#define MPFSHOST_CORE_CLIENT_CLIENT_TRANSPORT_HPP
#pragma once

#include "core/ipc/frame.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mpfshost::detail
{
    class FileDescriptor
    {
    public:
        explicit FileDescriptor(int fd);

        ~FileDescriptor();

        FileDescriptor(const FileDescriptor&)            = delete;
        FileDescriptor& operator=(const FileDescriptor&) = delete;

        FileDescriptor(FileDescriptor&& other) noexcept;
        FileDescriptor& operator=(FileDescriptor&& other) noexcept;

        [[nodiscard]] int fd() const noexcept;
        [[nodiscard]] int release() noexcept;

    private:
        int _fd;
    };

    [[nodiscard]] std::uint64_t nextRequestId();

    [[nodiscard]] int connectToServer(const std::filesystem::path& socketPath, std::string& error);

    struct Response
    {
        MessageType            type {};
        std::vector<std::byte> payload;
    };

    [[nodiscard]] std::optional<Response>
    exchange(int fd, MessageType type, std::span<const std::byte> payload, std::string& error);

    struct StreamedResponse
    {
        std::vector<std::vector<std::byte>> data;
        std::vector<std::byte>              endPayload;
    };

    [[nodiscard]] std::optional<StreamedResponse> exchangeStreamed(
        int fd, MessageType type, std::span<const std::byte> payload, MessageType endType, std::string& error);

    [[nodiscard]] std::optional<std::vector<std::byte>>
    expectResponse(const Response& response, MessageType expected, std::string& error, ErrorCode* errorCode = nullptr);

    [[nodiscard]] std::optional<Response> request(const std::filesystem::path& socketPath,
                                                  MessageType                  type,
                                                  std::span<const std::byte>   payload,
                                                  std::string&                 error);

    [[nodiscard]] std::optional<FileDescriptor>
    connectAndBind(const std::filesystem::path& socketPath, const std::string& targetName, std::string& error);
} // namespace mpfshost::detail

#endif // MPFSHOST_CORE_CLIENT_CLIENT_TRANSPORT_HPP
