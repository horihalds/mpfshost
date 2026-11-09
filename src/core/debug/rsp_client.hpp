#ifndef MPFSHOST_CORE_DEBUG_RSP_CLIENT_HPP
#define MPFSHOST_CORE_DEBUG_RSP_CLIENT_HPP

#pragma once

#include "core/ipc/message.hpp"

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

    enum class RspPollResult
    {
        Packet,  // a complete packet is in the out parameter
        Timeout, // no complete packet yet; the connection is still usable
        Closed,  // the peer went away or the stream is unusable
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
        // Closes the transport without destroying the state, so QEMU detaches the GDB client before the stub
        // is stopped and the guest is resumed.
        void               disconnect() noexcept;
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
        // Arms one hardware watchpoint. `unsupported` reports that the stub rejected a read or access watchpoint,
        // which the caller maps to CapabilityUnavailable instead of a generic stub failure.
        [[nodiscard]] bool insertWatchpoint(std::uint64_t             address,
                                            std::uint32_t             size,
                                            DebugWatchAccess          access,
                                            std::chrono::milliseconds timeout,
                                            std::string&              error,
                                            bool&                     unsupported);
        [[nodiscard]] bool removeWatchpoint(std::uint64_t             address,
                                            std::uint32_t             size,
                                            DebugWatchAccess          access,
                                            std::chrono::milliseconds timeout,
                                            std::string&              error);
        [[nodiscard]] bool
        insertBreakpoint(std::uint64_t address, bool hardware, std::chrono::milliseconds timeout, std::string& error);
        [[nodiscard]] bool
        removeBreakpoint(std::uint64_t address, bool hardware, std::chrono::milliseconds timeout, std::string& error);
        // Writes the continue packet and returns at once; pollPacket then waits for the stop reply.
        [[nodiscard]] bool continueExecution(std::chrono::milliseconds timeout, std::string& error);
        // Writes the single-step packet and returns at once; pollPacket then waits for the stop reply.
        [[nodiscard]] bool
        stepExecution(std::string_view thread, std::chrono::milliseconds timeout, std::string& error);
        // Reads whatever the stub has sent without blocking past the timeout. A partial packet is kept for the
        // next call, so a short poll does not lose a stop reply that arrives in pieces.
        [[nodiscard]] RspPollResult
             pollPacket(std::string& packet, std::chrono::milliseconds timeout, std::string& error);
        // Drops any packet already on the wire, such as a stop reply that arrived after the last resume, so a
        // teardown command is not answered by a stale reply. Never waits for new data.
        void drainPending();

        [[nodiscard]] bool               noAck() const noexcept;
        [[nodiscard]] const std::string& supported() const noexcept;

    private:
        [[nodiscard]] bool
        sendPacket(std::string_view data, std::string& reply, std::chrono::milliseconds timeout, std::string& error);
        [[nodiscard]] bool
        writePacketOnly(std::string_view data, std::chrono::milliseconds timeout, std::string& error);
        [[nodiscard]] bool
        checkAccepted(std::string_view command, std::chrono::milliseconds timeout, std::string& error);
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
