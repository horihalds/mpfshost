#include <catch2/catch.hpp>

#include "core/ipc/frame.hpp"
#include "core/ipc/message.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace
{
    class Pipe
    {
    public:
        Pipe()
        {
            int fds[2] {};
            REQUIRE(::pipe(fds) == 0);
            _readFd  = fds[0];
            _writeFd = fds[1];
        }

        ~Pipe()
        {
            closeRead();
            closeWrite();
        }

        Pipe(const Pipe&)            = delete;
        Pipe& operator=(const Pipe&) = delete;

        [[nodiscard]] int readFd() const noexcept
        {
            return _readFd;
        }

        [[nodiscard]] int writeFd() const noexcept
        {
            return _writeFd;
        }

        void write(std::span<const std::byte> bytes) const
        {
            std::size_t written = 0;
            while (written < bytes.size())
            {
                const auto result = ::write(_writeFd, bytes.data() + written, bytes.size() - written);
                REQUIRE(result > 0);
                written += static_cast<std::size_t>(result);
            }
        }

        void closeWrite()
        {
            if (_writeFd >= 0)
            {
                ::close(_writeFd);
                _writeFd = -1;
            }
        }

    private:
        void closeRead()
        {
            if (_readFd >= 0)
            {
                ::close(_readFd);
                _readFd = -1;
            }
        }

        int _readFd {-1};
        int _writeFd {-1};
    };

    class SocketPair
    {
    public:
        SocketPair()
        {
            int fds[2] {};
            REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
            _readFd  = fds[0];
            _writeFd = fds[1];
        }

        ~SocketPair()
        {
            if (_readFd >= 0)
            {
                ::close(_readFd);
            }
            if (_writeFd >= 0)
            {
                ::close(_writeFd);
            }
        }

        SocketPair(const SocketPair&)            = delete;
        SocketPair& operator=(const SocketPair&) = delete;

        [[nodiscard]] int readFd() const noexcept
        {
            return _readFd;
        }

        [[nodiscard]] int writeFd() const noexcept
        {
            return _writeFd;
        }

        void write(std::span<const std::byte> bytes) const
        {
            std::size_t written = 0;
            while (written < bytes.size())
            {
                const auto result = ::send(_writeFd, bytes.data() + written, bytes.size() - written, MSG_NOSIGNAL);
                REQUIRE(result > 0);
                written += static_cast<std::size_t>(result);
            }
        }

        void setTimeout(std::chrono::microseconds timeout) const
        {
            timeval value {};
            value.tv_sec  = static_cast<time_t>(timeout.count() / 1000000);
            value.tv_usec = static_cast<suseconds_t>(timeout.count() % 1000000);
            REQUIRE(::setsockopt(_readFd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) == 0);
        }

    private:
        int _readFd {-1};
        int _writeFd {-1};
    };

    std::vector<std::byte> rawFrame(std::uint32_t              magic,
                                    std::uint16_t              version,
                                    std::uint16_t              type,
                                    std::uint64_t              requestId,
                                    std::uint32_t              payloadLength,
                                    std::span<const std::byte> payload = {})
    {
        mpfshost::ByteWriter writer;
        writer.writeU32(magic);
        writer.writeU16(version);
        writer.writeU16(type);
        writer.writeU64(requestId);
        writer.writeU32(payloadLength);
        auto bytes = writer.take();
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        return bytes;
    }

    std::vector<std::byte> textBytes(const std::string& text)
    {
        std::vector<std::byte> bytes;
        bytes.reserve(text.size());
        for (const char value : text)
        {
            bytes.push_back(static_cast<std::byte>(value));
        }
        return bytes;
    }
} // namespace

TEST_CASE("frame round trip preserves the header and payload", "[ipc][frame]")
{
    const auto payload = textBytes("hello");
    const auto encoded = mpfshost::encodeFrame(mpfshost::MessageType::BindRequest, 42, payload);

    Pipe pipe;
    pipe.write(encoded);
    pipe.closeWrite();

    mpfshost::FrameHeader  header;
    std::vector<std::byte> decodedPayload;
    std::string            error;
    const auto             status = mpfshost::readFrame(pipe.readFd(), header, decodedPayload, error);

    REQUIRE(status == mpfshost::FrameReadStatus::Complete);
    REQUIRE(error.empty());
    REQUIRE(header.version == mpfshost::kProtocolVersion);
    REQUIRE(header.type == mpfshost::MessageType::BindRequest);
    REQUIRE(header.requestId == 42);
    REQUIRE(header.payloadLength == payload.size());
    REQUIRE(decodedPayload == payload);
}

TEST_CASE("readFrame reassembles a frame split across writes", "[ipc][frame]")
{
    const auto payload = textBytes("split payload");
    const auto encoded = mpfshost::encodeFrame(mpfshost::MessageType::ListTargetsRequest, 7, payload);

    Pipe pipe;
    pipe.write(std::span<const std::byte> {encoded.data(), 7});
    pipe.write(std::span<const std::byte> {encoded.data() + 7, 5});
    pipe.write(std::span<const std::byte> {encoded.data() + 12, encoded.size() - 12});
    pipe.closeWrite();

    mpfshost::FrameHeader  header;
    std::vector<std::byte> decodedPayload;
    std::string            error;
    const auto             status = mpfshost::readFrame(pipe.readFd(), header, decodedPayload, error);

    REQUIRE(status == mpfshost::FrameReadStatus::Complete);
    REQUIRE(header.requestId == 7);
    REQUIRE(decodedPayload == payload);
}

TEST_CASE("readFrame reports a clean EOF as a closed connection", "[ipc][frame]")
{
    Pipe pipe;
    pipe.closeWrite();

    mpfshost::FrameHeader  header;
    std::vector<std::byte> payload;
    std::string            error;
    const auto             status = mpfshost::readFrame(pipe.readFd(), header, payload, error);

    REQUIRE(status == mpfshost::FrameReadStatus::Closed);
    REQUIRE(error.empty());
}

TEST_CASE("readFrame treats a truncated header as a closed connection", "[ipc][frame]")
{
    const auto encoded = mpfshost::encodeFrame(mpfshost::MessageType::ListTargetsRequest, 1, {});

    Pipe pipe;
    pipe.write(std::span<const std::byte> {encoded.data(), 5});
    pipe.closeWrite();

    mpfshost::FrameHeader  header;
    std::vector<std::byte> payload;
    std::string            error;
    const auto             status = mpfshost::readFrame(pipe.readFd(), header, payload, error);

    REQUIRE(status == mpfshost::FrameReadStatus::Closed);
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("readFrame reports a receive timeout distinct from a closed connection", "[ipc][frame]")
{
    SocketPair socket;
    socket.setTimeout(std::chrono::milliseconds {50});

    mpfshost::FrameHeader  header;
    std::vector<std::byte> payload;
    std::string            error;
    const auto             status = mpfshost::readFrame(socket.readFd(), header, payload, error);

    REQUIRE(status == mpfshost::FrameReadStatus::Timeout);
    REQUIRE_FALSE(error.empty());
    REQUIRE(payload.empty());
}

TEST_CASE("readFrame reports a timeout after part of the payload arrived", "[ipc][frame]")
{
    const auto encoded = mpfshost::encodeFrame(mpfshost::MessageType::BindRequest, 3, textBytes("payload"));

    SocketPair socket;
    socket.setTimeout(std::chrono::milliseconds {50});
    socket.write(std::span<const std::byte> {encoded.data(), mpfshost::kFrameHeaderSize + 3});

    mpfshost::FrameHeader  header;
    std::vector<std::byte> payload;
    std::string            error;
    const auto             status = mpfshost::readFrame(socket.readFd(), header, payload, error);

    REQUIRE(status == mpfshost::FrameReadStatus::Timeout);
    REQUIRE_FALSE(error.empty());
    REQUIRE(payload.empty());
}

TEST_CASE("bufferedFrameLength reports nothing without a complete frame", "[ipc][frame]")
{
    SocketPair socket;
    REQUIRE_FALSE(mpfshost::bufferedFrameLength(socket.readFd()).has_value());

    const auto encoded = mpfshost::encodeFrame(mpfshost::MessageType::CancelRequest, 1, textBytes("12345678"));
    socket.write(std::span<const std::byte> {encoded.data(), 10});
    REQUIRE_FALSE(mpfshost::bufferedFrameLength(socket.readFd()).has_value());
}

TEST_CASE("bufferedFrameLength waits for the whole announced payload", "[ipc][frame]")
{
    const auto payload = textBytes("abcdefgh");
    const auto encoded = mpfshost::encodeFrame(mpfshost::MessageType::CancelRequest, 1, payload);

    SocketPair socket;
    socket.write(std::span<const std::byte> {encoded.data(), mpfshost::kFrameHeaderSize + 4});
    REQUIRE_FALSE(mpfshost::bufferedFrameLength(socket.readFd()).has_value());

    socket.write(std::span<const std::byte> {encoded.data() + mpfshost::kFrameHeaderSize + 4, 4});
    const auto length = mpfshost::bufferedFrameLength(socket.readFd());
    REQUIRE(length.has_value());
    REQUIRE(*length == payload.size());
}

TEST_CASE("bufferedFrameLength returns the first frame when several are buffered", "[ipc][frame]")
{
    const auto first  = mpfshost::encodeFrame(mpfshost::MessageType::CancelRequest, 1, textBytes("1234"));
    const auto second = mpfshost::encodeFrame(mpfshost::MessageType::ListTargetsRequest, 2, textBytes("abcdefgh"));

    SocketPair socket;
    socket.write(first);
    socket.write(second);

    const auto length = mpfshost::bufferedFrameLength(socket.readFd());
    REQUIRE(length.has_value());
    REQUIRE(*length == 4);

    mpfshost::FrameHeader  header;
    std::vector<std::byte> payload;
    std::string            error;
    REQUIRE(mpfshost::readFrame(socket.readFd(), header, payload, error) == mpfshost::FrameReadStatus::Complete);
    REQUIRE(header.requestId == 1);
    REQUIRE(payload == textBytes("1234"));
}

TEST_CASE("bufferedFrameLength reports an empty payload as zero", "[ipc][frame]")
{
    const auto encoded = mpfshost::encodeFrame(mpfshost::MessageType::ListTargetsRequest, 5, {});

    SocketPair socket;
    socket.write(encoded);

    const auto length = mpfshost::bufferedFrameLength(socket.readFd());
    REQUIRE(length.has_value());
    REQUIRE(*length == 0);
}

TEST_CASE("readFrame rejects a bad magic without reporting a header", "[ipc][frame]")
{
    const auto encoded = rawFrame(mpfshost::kProtocolMagic + 1,
                                  mpfshost::kProtocolVersion,
                                  static_cast<std::uint16_t>(mpfshost::MessageType::ListTargetsRequest),
                                  5,
                                  0);

    Pipe pipe;
    pipe.write(encoded);
    pipe.closeWrite();

    mpfshost::FrameHeader  header;
    std::vector<std::byte> payload;
    std::string            error;
    const auto             status = mpfshost::readFrame(pipe.readFd(), header, payload, error);

    REQUIRE(status == mpfshost::FrameReadStatus::Closed);
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("readFrame rejects an unsupported protocol version but keeps the header", "[ipc][frame]")
{
    const auto encoded = rawFrame(mpfshost::kProtocolMagic,
                                  mpfshost::kProtocolVersion + 1,
                                  static_cast<std::uint16_t>(mpfshost::MessageType::BindRequest),
                                  9,
                                  0);

    Pipe pipe;
    pipe.write(encoded);
    pipe.closeWrite();

    mpfshost::FrameHeader  header;
    std::vector<std::byte> payload;
    std::string            error;
    const auto             status = mpfshost::readFrame(pipe.readFd(), header, payload, error);

    REQUIRE(status == mpfshost::FrameReadStatus::Rejected);
    REQUIRE(header.version == mpfshost::kProtocolVersion + 1);
    REQUIRE(header.type == mpfshost::MessageType::BindRequest);
    REQUIRE(header.requestId == 9);
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("readFrame rejects a payload over the cap before allocating it", "[ipc][frame]")
{
    const auto encoded = rawFrame(mpfshost::kProtocolMagic,
                                  mpfshost::kProtocolVersion,
                                  static_cast<std::uint16_t>(mpfshost::MessageType::ListTargetsRequest),
                                  3,
                                  mpfshost::kMaxPayloadLength + 1);

    Pipe pipe;
    pipe.write(encoded);
    pipe.closeWrite();

    mpfshost::FrameHeader  header;
    std::vector<std::byte> payload;
    std::string            error;
    const auto             status = mpfshost::readFrame(pipe.readFd(), header, payload, error);

    REQUIRE(status == mpfshost::FrameReadStatus::Rejected);
    REQUIRE(header.payloadLength == mpfshost::kMaxPayloadLength + 1);
    REQUIRE(payload.empty());
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("readFrame treats a truncated payload as a closed connection", "[ipc][frame]")
{
    const auto payload = textBytes("abcd");
    const auto encoded = rawFrame(mpfshost::kProtocolMagic,
                                  mpfshost::kProtocolVersion,
                                  static_cast<std::uint16_t>(mpfshost::MessageType::BindRequest),
                                  4,
                                  10,
                                  payload);

    Pipe pipe;
    pipe.write(encoded);
    pipe.closeWrite();

    mpfshost::FrameHeader  header;
    std::vector<std::byte> decodedPayload;
    std::string            error;
    const auto             status = mpfshost::readFrame(pipe.readFd(), header, decodedPayload, error);

    REQUIRE(status == mpfshost::FrameReadStatus::Closed);
    REQUIRE(decodedPayload.empty());
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("writeFrame produces a frame readable by readFrame", "[ipc][frame]")
{
    const auto payload = textBytes("written");

    Pipe        pipe;
    std::string writeError;
    REQUIRE(mpfshost::writeFrame(pipe.writeFd(), mpfshost::MessageType::ErrorResponse, 11, payload, writeError));
    REQUIRE(writeError.empty());
    pipe.closeWrite();

    mpfshost::FrameHeader  header;
    std::vector<std::byte> decodedPayload;
    std::string            readError;
    const auto             status = mpfshost::readFrame(pipe.readFd(), header, decodedPayload, readError);

    REQUIRE(status == mpfshost::FrameReadStatus::Complete);
    REQUIRE(header.type == mpfshost::MessageType::ErrorResponse);
    REQUIRE(header.requestId == 11);
    REQUIRE(decodedPayload == payload);
}

TEST_CASE("ByteReader latches an underrun instead of throwing", "[ipc][frame]")
{
    const std::vector<std::byte> empty;
    mpfshost::ByteReader         reader {empty};

    REQUIRE(reader.readU32() == 0);
    REQUIRE_FALSE(reader.ok());
    REQUIRE(reader.readString().empty());
    REQUIRE_FALSE(reader.ok());
    REQUIRE(reader.readU64() == 0);
}

TEST_CASE("ByteReader rejects a string longer than the remaining payload", "[ipc][frame]")
{
    mpfshost::ByteWriter writer;
    writer.writeU32(64);
    const auto bytes = writer.take();

    mpfshost::ByteReader reader {bytes};
    REQUIRE(reader.readString().empty());
    REQUIRE_FALSE(reader.ok());
}

TEST_CASE("message payloads round trip", "[ipc][message]")
{
    mpfshost::ListTargetsResponse list;
    list.targets.push_back({"alpha", 111, "/var/qmp-sockets/alpha.sock"});
    list.targets.push_back({"beta", 222, "/var/qmp-sockets/beta.sock"});

    const auto listBytes   = mpfshost::encodeListTargetsResponse(list);
    const auto listDecoded = mpfshost::decodeListTargetsResponse(listBytes);
    REQUIRE(listDecoded.has_value());
    REQUIRE(listDecoded->targets.size() == 2);
    REQUIRE(listDecoded->targets[0].name == "alpha");
    REQUIRE(listDecoded->targets[0].qemuPid == 111);
    REQUIRE(listDecoded->targets[0].qmpSocketPath == "/var/qmp-sockets/alpha.sock");
    REQUIRE(listDecoded->targets[1].name == "beta");

    const auto bindRequestBytes   = mpfshost::encodeBindRequest({"alpha"});
    const auto bindRequestDecoded = mpfshost::decodeBindRequest(bindRequestBytes);
    REQUIRE(bindRequestDecoded.has_value());
    REQUIRE(bindRequestDecoded->targetName == "alpha");

    const auto bindResponseBytes   = mpfshost::encodeBindResponse({
        77, {"alpha", 111, "/var/qmp-sockets/alpha.sock"}
    });
    const auto bindResponseDecoded = mpfshost::decodeBindResponse(bindResponseBytes);
    REQUIRE(bindResponseDecoded.has_value());
    REQUIRE(bindResponseDecoded->sessionId == 77);
    REQUIRE(bindResponseDecoded->target.name == "alpha");
    REQUIRE(bindResponseDecoded->target.qemuPid == 111);

    const auto errorBytes   = mpfshost::encodeErrorResponse({mpfshost::ErrorCode::TargetNotFound, "missing"});
    const auto errorDecoded = mpfshost::decodeErrorResponse(errorBytes);
    REQUIRE(errorDecoded.has_value());
    REQUIRE(errorDecoded->code == mpfshost::ErrorCode::TargetNotFound);
    REQUIRE(errorDecoded->message == "missing");
}

TEST_CASE("physical memory message types and limits are declared", "[ipc][frame]")
{
    REQUIRE(static_cast<std::uint16_t>(mpfshost::MessageType::VmListRequest) == 0x000D);
    REQUIRE(static_cast<std::uint16_t>(mpfshost::MessageType::PhysicalReadRequest) == 0x000E);
    REQUIRE(static_cast<std::uint16_t>(mpfshost::MessageType::PhysicalWriteRequest) == 0x000F);
    REQUIRE(static_cast<std::uint16_t>(mpfshost::MessageType::GpaTranslateRequest) == 0x0010);
    REQUIRE(static_cast<std::uint16_t>(mpfshost::MessageType::Virt2PhysRequest) == 0x0011);
    REQUIRE(static_cast<std::uint16_t>(mpfshost::MessageType::PhysicalScatterRequest) == 0x0012);
    REQUIRE(static_cast<std::uint16_t>(mpfshost::MessageType::VmListResponse) == 0x8012);
    REQUIRE(static_cast<std::uint16_t>(mpfshost::MessageType::PhysicalReadData) == 0x8013);
    REQUIRE(static_cast<std::uint16_t>(mpfshost::MessageType::PhysicalReadEnd) == 0x8014);
    REQUIRE(static_cast<std::uint16_t>(mpfshost::MessageType::PhysicalWriteResponse) == 0x8015);
    REQUIRE(static_cast<std::uint16_t>(mpfshost::MessageType::GpaTranslateResponse) == 0x8016);
    REQUIRE(static_cast<std::uint16_t>(mpfshost::MessageType::Virt2PhysResponse) == 0x8017);
    REQUIRE(static_cast<std::uint16_t>(mpfshost::MessageType::PhysicalScatterData) == 0x8018);
    REQUIRE(static_cast<std::uint16_t>(mpfshost::MessageType::PhysicalScatterEnd) == 0x8019);
    REQUIRE(static_cast<std::uint16_t>(mpfshost::ErrorCode::UnknownVm) == 14);

    REQUIRE(mpfshost::kPhysicalScatterPageSize == 0x1000);
    REQUIRE(mpfshost::kPhysicalScatterMinBytes == 8);
    REQUIRE(mpfshost::kMaxPhysicalScatterEntries == mpfshost::kMaxScatterEntries);
    REQUIRE(mpfshost::kMaxPhysicalScatterBytes == mpfshost::kMaxScatterReadBytes);
}

TEST_CASE("cancellation message types and error codes are declared", "[ipc][frame]")
{
    REQUIRE(static_cast<std::uint16_t>(mpfshost::MessageType::CancelRequest) == 0x0018);
    REQUIRE(static_cast<std::uint16_t>(mpfshost::MessageType::CancelResponse) == 0x8021);
    REQUIRE(static_cast<std::uint16_t>(mpfshost::ErrorCode::Cancelled) == 17);
    REQUIRE(static_cast<std::uint16_t>(mpfshost::ErrorCode::Timeout) == 18);
    REQUIRE(static_cast<std::uint16_t>(mpfshost::ErrorCode::Busy) == 19);
    REQUIRE(mpfshost::kProtocolVersion == 2);
}

TEST_CASE("malformed message payloads are rejected", "[ipc][message]")
{
    mpfshost::ByteWriter truncatedString;
    truncatedString.writeU32(64);
    REQUIRE_FALSE(mpfshost::decodeBindRequest(truncatedString.bytes()).has_value());

    mpfshost::ByteWriter trailingData;
    trailingData.writeString("alpha");
    trailingData.writeU16(0);
    REQUIRE_FALSE(mpfshost::decodeBindRequest(trailingData.bytes()).has_value());

    mpfshost::ByteWriter absurdCount;
    absurdCount.writeU32(0xFFFFFFFFu);
    REQUIRE_FALSE(mpfshost::decodeListTargetsResponse(absurdCount.bytes()).has_value());
}

TEST_CASE("message type and error code names cover every enumerator", "[ipc][frame]")
{
    const std::vector<mpfshost::MessageType> types {
        mpfshost::MessageType::ListTargetsRequest,
        mpfshost::MessageType::BindRequest,
        mpfshost::MessageType::ProcessListRequest,
        mpfshost::MessageType::MemoryReadRequest,
        mpfshost::MessageType::MemoryWriteRequest,
        mpfshost::MessageType::ScatterCreateRequest,
        mpfshost::MessageType::ScatterPrepareRequest,
        mpfshost::MessageType::ScatterExecuteRequest,
        mpfshost::MessageType::ScatterCloseRequest,
        mpfshost::MessageType::ModuleListRequest,
        mpfshost::MessageType::MemoryMapRequest,
        mpfshost::MessageType::ThreadListRequest,
        mpfshost::MessageType::VmListRequest,
        mpfshost::MessageType::PhysicalReadRequest,
        mpfshost::MessageType::PhysicalWriteRequest,
        mpfshost::MessageType::GpaTranslateRequest,
        mpfshost::MessageType::Virt2PhysRequest,
        mpfshost::MessageType::PhysicalScatterRequest,
        mpfshost::MessageType::VfsListRequest,
        mpfshost::MessageType::VfsReadRequest,
        mpfshost::MessageType::VfsWriteRequest,
        mpfshost::MessageType::CancelRequest,
        mpfshost::MessageType::ListTargetsResponse,
        mpfshost::MessageType::BindResponse,
        mpfshost::MessageType::ProcessListResponse,
        mpfshost::MessageType::MemoryReadData,
        mpfshost::MessageType::MemoryReadEnd,
        mpfshost::MessageType::MemoryWriteResponse,
        mpfshost::MessageType::ScatterCreateResponse,
        mpfshost::MessageType::ScatterPrepareResponse,
        mpfshost::MessageType::ScatterExecuteData,
        mpfshost::MessageType::ScatterExecuteEnd,
        mpfshost::MessageType::ScatterCloseResponse,
        mpfshost::MessageType::ModuleListData,
        mpfshost::MessageType::ModuleListEnd,
        mpfshost::MessageType::MemoryMapData,
        mpfshost::MessageType::MemoryMapEnd,
        mpfshost::MessageType::ThreadListData,
        mpfshost::MessageType::ThreadListEnd,
        mpfshost::MessageType::VmListResponse,
        mpfshost::MessageType::PhysicalReadData,
        mpfshost::MessageType::PhysicalReadEnd,
        mpfshost::MessageType::PhysicalWriteResponse,
        mpfshost::MessageType::GpaTranslateResponse,
        mpfshost::MessageType::Virt2PhysResponse,
        mpfshost::MessageType::PhysicalScatterData,
        mpfshost::MessageType::PhysicalScatterEnd,
        mpfshost::MessageType::VfsListData,
        mpfshost::MessageType::VfsListEnd,
        mpfshost::MessageType::VfsReadData,
        mpfshost::MessageType::VfsReadEnd,
        mpfshost::MessageType::VfsWriteResponse,
        mpfshost::MessageType::CancelResponse,
        mpfshost::MessageType::ErrorResponse,
    };

    std::vector<std::string> names;
    names.reserve(types.size());
    for (const auto type : types)
    {
        const auto name = mpfshost::messageTypeName(type);
        REQUIRE_FALSE(name.empty());
        CHECK(name != "UnknownMessageType");
        names.emplace_back(name);
    }
    std::sort(names.begin(), names.end());
    CHECK(std::adjacent_find(names.begin(), names.end()) == names.end());

    const std::vector<mpfshost::ErrorCode> codes {
        mpfshost::ErrorCode::MalformedPayload,
        mpfshost::ErrorCode::UnsupportedVersion,
        mpfshost::ErrorCode::UnknownMessage,
        mpfshost::ErrorCode::TargetNotFound,
        mpfshost::ErrorCode::TargetUnavailable,
        mpfshost::ErrorCode::SessionAlreadyBound,
        mpfshost::ErrorCode::Internal,
        mpfshost::ErrorCode::NotBound,
        mpfshost::ErrorCode::CapabilityUnavailable,
        mpfshost::ErrorCode::ResultTooLarge,
        mpfshost::ErrorCode::UnknownScatter,
        mpfshost::ErrorCode::InvalidRange,
        mpfshost::ErrorCode::ScatterLimitReached,
        mpfshost::ErrorCode::UnknownVm,
        mpfshost::ErrorCode::Cancelled,
        mpfshost::ErrorCode::Timeout,
        mpfshost::ErrorCode::Busy,
    };

    std::vector<std::string> codeNames;
    codeNames.reserve(codes.size());
    for (const auto code : codes)
    {
        const auto name = mpfshost::errorCodeName(code);
        REQUIRE_FALSE(name.empty());
        CHECK(name != "UnknownErrorCode");
        codeNames.emplace_back(name);
    }
    std::sort(codeNames.begin(), codeNames.end());
    CHECK(std::adjacent_find(codeNames.begin(), codeNames.end()) == codeNames.end());

    CHECK(mpfshost::messageTypeName(static_cast<mpfshost::MessageType>(0xFFFF)) == "UnknownMessageType");
    CHECK(mpfshost::errorCodeName(static_cast<mpfshost::ErrorCode>(0xFFFF)) == "UnknownErrorCode");
}
