#ifndef MPFSHOST_CORE_DEBUG_RSP_CLIENT_HPP
#define MPFSHOST_CORE_DEBUG_RSP_CLIENT_HPP

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mpfshost::debug
{
    struct RspEndpoint
    {
        enum class Kind
        {
            Unix,
            Tcp,
        };

        Kind                  kind {Kind::Unix};
        std::filesystem::path unixPath;
        std::string           tcpHost;
        std::uint16_t         tcpPort {};
    };

    // Byte transport behind the GDB remote protocol. Tests script raw packets with a fake.
    class RspTransport
    {
    public:
        virtual ~RspTransport() = default;

        [[nodiscard]] virtual bool
        open(const RspEndpoint& endpoint, std::chrono::milliseconds timeout, std::string& error) = 0;
        // Returns the number of bytes read, 0 on peer EOF, or -1 with error set.
        [[nodiscard]] virtual std::ptrdiff_t
        readSome(std::span<std::byte> buffer, std::chrono::milliseconds timeout, std::string& error) = 0;
        [[nodiscard]] virtual bool
        writeAll(std::span<const std::byte> data, std::chrono::milliseconds timeout, std::string& error) = 0;
        virtual void close() noexcept                                                                    = 0;
    };

    [[nodiscard]] std::unique_ptr<RspTransport> makeSocketRspTransport();

    struct RspRegisterInfo
    {
        std::string   name;
        std::uint32_t bitsize {};
        std::uint32_t number {};
        std::uint32_t offset {}; // byte offset in the `g` packet
    };

    struct RspTargetDescription
    {
        std::vector<RspRegisterInfo> registers;
        std::string                  xml; // target.xml plus every fetched include, concatenated
    };

    // Parses `<reg>` elements in document order, honouring an explicit `regnum` and otherwise numbering
    // sequentially. XML comments are ignored, so a commented-out register does not shift the layout.
    [[nodiscard]] RspTargetDescription           parseRspTargetDescription(const std::string& xml);
    [[nodiscard]] std::optional<RspRegisterInfo> findRspRegister(const RspTargetDescription& description,
                                                                 std::string_view            name);
    [[nodiscard]] std::optional<RspRegisterInfo> fallbackRspRegister(std::string_view name);
    [[nodiscard]] std::optional<std::uint64_t>   readRspRegisterValue(std::span<const std::byte> registers,
                                                                      const RspRegisterInfo&     info);

    class RspClient
    {
    public:
        explicit RspClient(std::unique_ptr<RspTransport> transport);
        ~RspClient();

        RspClient(const RspClient&)            = delete;
        RspClient& operator=(const RspClient&) = delete;

        [[nodiscard]] bool connect(const RspEndpoint& endpoint, std::chrono::milliseconds timeout, std::string& error);
        // Sends qSupported and, when the stub offers it, enters no-ack mode.
        [[nodiscard]] bool negotiate(std::chrono::milliseconds timeout, std::string& error);
        [[nodiscard]] bool
        readTargetDescription(RspTargetDescription& description, std::chrono::milliseconds timeout, std::string& error);
        [[nodiscard]] bool
        queryThreads(std::vector<std::string>& threads, std::chrono::milliseconds timeout, std::string& error);
        [[nodiscard]] bool selectThread(std::string_view thread, std::chrono::milliseconds timeout, std::string& error);
        [[nodiscard]] bool
        readAllRegisters(std::vector<std::byte>& registers, std::chrono::milliseconds timeout, std::string& error);
        [[nodiscard]] bool
        readRegister(std::uint32_t number, std::uint64_t& value, std::chrono::milliseconds timeout, std::string& error);

        [[nodiscard]] bool               noAck() const noexcept;
        [[nodiscard]] const std::string& supported() const noexcept;

    private:
        [[nodiscard]] bool
        sendPacket(std::string_view data, std::string& reply, std::chrono::milliseconds timeout, std::string& error);
        [[nodiscard]] bool
        readByte(std::chrono::steady_clock::time_point deadline, unsigned char& value, std::string& error);
        [[nodiscard]] bool
        writeRaw(std::span<const std::byte> data, std::chrono::steady_clock::time_point deadline, std::string& error);
        [[nodiscard]] bool fetchFile(std::string_view                      annex,
                                     std::string&                          content,
                                     std::chrono::steady_clock::time_point deadline,
                                     std::string&                          error);

        std::unique_ptr<RspTransport> _transport;
        std::string                   _buffer;
        std::string                   _supported;
        bool                          _noAck {false};
    };
} // namespace mpfshost::debug

#endif // MPFSHOST_CORE_DEBUG_RSP_CLIENT_HPP
