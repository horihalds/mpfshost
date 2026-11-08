#include "core/debug/rsp_client.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <regex>
#include <span>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace mpfshost::debug
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr std::size_t kMaxRspFileBytes = 1u << 20;
        constexpr std::size_t kMaxRspFiles     = 16;
        constexpr std::size_t kMaxRspThreads   = 1024;

        std::chrono::milliseconds remaining(Clock::time_point deadline)
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            return left > std::chrono::milliseconds::zero() ? left : std::chrono::milliseconds::zero();
        }

        bool
        waitForDescriptor(int fd, short events, std::chrono::milliseconds timeout, bool& closed, std::string& error)
        {
            const auto deadline = Clock::now() + timeout;
            while (true)
            {
                const auto left = remaining(deadline);
                if (left == std::chrono::milliseconds::zero())
                {
                    error = "the GDB stub socket timed out";
                    return false;
                }

                pollfd     descriptor {fd, events, 0};
                const auto result = ::poll(&descriptor, 1, static_cast<int>(left.count()));
                if (result == 0)
                {
                    error = "the GDB stub socket timed out";
                    return false;
                }
                if (result < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    error = std::string("poll on the GDB stub socket failed: ") + std::strerror(errno);
                    return false;
                }
                if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0)
                {
                    error = "the GDB stub socket reported an error";
                    return false;
                }
                if ((descriptor.revents & POLLHUP) != 0)
                {
                    closed = true;
                }
                return true;
            }
        }

        class SocketRspTransport final : public RspTransport
        {
        public:
            ~SocketRspTransport() override
            {
                close();
            }

            [[nodiscard]] bool
            open(const RspEndpoint& endpoint, std::chrono::milliseconds timeout, std::string& error) override
            {
                close();
                if (endpoint.kind == RspEndpoint::Kind::Unix)
                {
                    return openUnix(endpoint.unixPath, timeout, error);
                }
                return openTcp(endpoint, timeout, error);
            }

            [[nodiscard]] std::ptrdiff_t
            readSome(std::span<std::byte> buffer, std::chrono::milliseconds timeout, std::string& error) override
            {
                const auto deadline = Clock::now() + timeout;
                while (true)
                {
                    if (_fd < 0)
                    {
                        error = "the GDB stub socket is not open";
                        return -1;
                    }
                    const auto left = remaining(deadline);
                    if (left == std::chrono::milliseconds::zero())
                    {
                        error = "the GDB stub socket timed out";
                        return -1;
                    }
                    bool closed = false;
                    if (!waitForDescriptor(_fd, POLLIN, left, closed, error))
                    {
                        return -1;
                    }
                    if (closed)
                    {
                        return 0;
                    }

                    const auto received = ::recv(_fd, buffer.data(), buffer.size(), 0);
                    if (received > 0)
                    {
                        return received;
                    }
                    if (received == 0)
                    {
                        return 0;
                    }
                    if (errno == EINTR || errno == EAGAIN)
                    {
                        continue;
                    }
                    error = std::string("reading from the GDB stub socket failed: ") + std::strerror(errno);
                    return -1;
                }
            }

            [[nodiscard]] bool
            writeAll(std::span<const std::byte> data, std::chrono::milliseconds timeout, std::string& error) override
            {
                const auto  deadline = Clock::now() + timeout;
                std::size_t offset   = 0;
                while (offset < data.size())
                {
                    if (_fd < 0)
                    {
                        error = "the GDB stub socket is not open";
                        return false;
                    }
                    const auto left = remaining(deadline);
                    if (left == std::chrono::milliseconds::zero())
                    {
                        error = "the GDB stub socket timed out while writing";
                        return false;
                    }
                    bool closed = false;
                    if (!waitForDescriptor(_fd, POLLOUT, left, closed, error))
                    {
                        return false;
                    }
                    if (closed)
                    {
                        error = "the GDB stub connection closed while writing";
                        return false;
                    }

                    const auto written = ::send(_fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
                    if (written < 0)
                    {
                        if (errno == EINTR || errno == EAGAIN)
                        {
                            continue;
                        }
                        error = std::string("writing to the GDB stub socket failed: ") + std::strerror(errno);
                        return false;
                    }
                    offset += static_cast<std::size_t>(written);
                }
                return true;
            }

            void close() noexcept override
            {
                if (_fd >= 0)
                {
                    ::close(_fd);
                    _fd = -1;
                }
            }

        private:
            [[nodiscard]] bool
            openUnix(const std::filesystem::path& socketPath, std::chrono::milliseconds timeout, std::string& error)
            {
                const auto path = socketPath.string();
                if (path.empty())
                {
                    error = "the GDB stub socket path is empty";
                    return false;
                }

                sockaddr_un address {};
                if (path.size() >= sizeof(address.sun_path))
                {
                    error = "the GDB stub socket path is too long";
                    return false;
                }

                const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
                if (fd < 0)
                {
                    error = std::string("socket for the GDB stub failed: ") + std::strerror(errno);
                    return false;
                }

                address.sun_family = AF_UNIX;
                std::memcpy(address.sun_path, path.c_str(), path.size() + 1);

                if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
                {
                    if (errno != EINPROGRESS)
                    {
                        error = "connect to the GDB stub failed: " + std::string(std::strerror(errno));
                        ::close(fd);
                        return false;
                    }
                    bool closed = false;
                    if (!waitForDescriptor(fd, POLLOUT, timeout, closed, error))
                    {
                        if (error.empty())
                        {
                            error = "the GDB stub socket closed while connecting";
                        }
                        ::close(fd);
                        return false;
                    }
                    int       socketError = 0;
                    socklen_t length      = sizeof(socketError);
                    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &length) != 0 || socketError != 0)
                    {
                        error = "connect to the GDB stub failed: " + std::string(std::strerror(socketError));
                        ::close(fd);
                        return false;
                    }
                }

                _fd = fd;
                return true;
            }

            [[nodiscard]] bool
            openTcp(const RspEndpoint& endpoint, std::chrono::milliseconds timeout, std::string& error)
            {
                const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
                if (fd < 0)
                {
                    error = std::string("socket for the GDB stub failed: ") + std::strerror(errno);
                    return false;
                }

                sockaddr_in address {};
                address.sin_family = AF_INET;
                address.sin_port   = htons(endpoint.tcpPort);
                if (::inet_pton(AF_INET, endpoint.tcpHost.c_str(), &address.sin_addr) != 1)
                {
                    error = "the GDB TCP host is not an IPv4 address";
                    ::close(fd);
                    return false;
                }

                if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
                {
                    if (errno != EINPROGRESS)
                    {
                        error = "connect to the GDB stub failed: " + std::string(std::strerror(errno));
                        ::close(fd);
                        return false;
                    }
                    bool closed = false;
                    if (!waitForDescriptor(fd, POLLOUT, timeout, closed, error))
                    {
                        if (error.empty())
                        {
                            error = "the GDB stub socket closed while connecting";
                        }
                        ::close(fd);
                        return false;
                    }
                    int       socketError = 0;
                    socklen_t length      = sizeof(socketError);
                    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &length) != 0 || socketError != 0)
                    {
                        error = "connect to the GDB stub failed: " + std::string(std::strerror(socketError));
                        ::close(fd);
                        return false;
                    }
                }

                _fd = fd;
                return true;
            }

            int _fd {-1};
        };

        std::optional<std::uint32_t> parseU32(std::string_view text)
        {
            std::uint32_t value {};
            const auto    result = std::from_chars(text.data(), text.data() + text.size(), value);
            if (result.ec != std::errc {} || result.ptr != text.data() + text.size())
            {
                return std::nullopt;
            }
            return value;
        }

        int hexDigit(char value)
        {
            if (value >= '0' && value <= '9')
            {
                return value - '0';
            }
            if (value >= 'a' && value <= 'f')
            {
                return value - 'a' + 10;
            }
            if (value >= 'A' && value <= 'F')
            {
                return value - 'A' + 10;
            }
            return -1;
        }

        std::optional<std::vector<std::byte>> decodeHex(std::string_view text)
        {
            if (text.size() % 2 != 0)
            {
                return std::nullopt;
            }
            std::vector<std::byte> bytes;
            bytes.reserve(text.size() / 2);
            for (std::size_t index = 0; index < text.size(); index += 2)
            {
                const int high = hexDigit(text[index]);
                const int low  = hexDigit(text[index + 1]);
                if (high < 0 || low < 0)
                {
                    return std::nullopt;
                }
                bytes.push_back(static_cast<std::byte>((high << 4) | low));
            }
            return bytes;
        }

        std::string buildPacket(std::string_view data)
        {
            std::string packet {"$"};
            packet.append(data);
            packet.push_back('#');
            unsigned sum = 0;
            for (const char value : data)
            {
                sum += static_cast<unsigned char>(value);
            }
            static constexpr char digits[] = "0123456789abcdef";
            packet.push_back(digits[(sum >> 4) & 0x0F]);
            packet.push_back(digits[sum & 0x0F]);
            return packet;
        }

        const std::array<RspRegisterInfo, 22>& fallbackTable()
        {
            static const std::array<RspRegisterInfo, 22> table {
                {
                 {"rax", 64, 0, 0},    {"rbx", 64, 1, 8},       {"rcx", 64, 2, 16},   {"rdx", 64, 3, 24},
                 {"rsi", 64, 4, 32},   {"rdi", 64, 5, 40},      {"rbp", 64, 6, 48},   {"rsp", 64, 7, 56},
                 {"r8", 64, 8, 64},    {"r9", 64, 9, 72},       {"r10", 64, 10, 80},  {"r11", 64, 11, 88},
                 {"r12", 64, 12, 96},  {"r13", 64, 13, 104},    {"r14", 64, 14, 112}, {"r15", 64, 15, 120},
                 {"rip", 64, 16, 128}, {"eflags", 32, 17, 136}, {"cr0", 64, 27, 188}, {"cr3", 64, 29, 204},
                 {"cr4", 64, 30, 212}, {"cr8", 64, 31, 220},
                 }
            };
            return table;
        }
    } // namespace

    std::unique_ptr<RspTransport> makeSocketRspTransport()
    {
        return std::make_unique<SocketRspTransport>();
    }

    RspTargetDescription parseRspTargetDescription(const std::string& xml)
    {
        static const std::regex commentPattern("<!--[\\s\\S]*?-->");
        static const std::regex regPattern("<reg\\b([^>]*)>");
        static const std::regex attrPattern("([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*\"([^\"]*)\"");

        const auto stripped = std::regex_replace(xml, commentPattern, "");

        RspTargetDescription description;
        std::uint32_t        nextNumber = 0;
        std::uint32_t        offset     = 0;
        for (std::sregex_iterator it(stripped.begin(), stripped.end(), regPattern), end; it != end; ++it)
        {
            const auto  attributes = (*it)[1].str();
            std::string name;
            std::string bitsizeText;
            std::string regnumText;
            for (std::sregex_iterator attr(attributes.begin(), attributes.end(), attrPattern), attrEnd; attr != attrEnd;
                 ++attr)
            {
                const auto key   = (*attr)[1].str();
                const auto value = (*attr)[2].str();
                if (key == "name")
                {
                    name = value;
                }
                else if (key == "bitsize")
                {
                    bitsizeText = value;
                }
                else if (key == "regnum")
                {
                    regnumText = value;
                }
            }
            if (name.empty() || bitsizeText.empty())
            {
                continue;
            }

            const auto bitsize = parseU32(bitsizeText);
            if (!bitsize || *bitsize == 0)
            {
                continue;
            }

            RspRegisterInfo info;
            info.name    = name;
            info.bitsize = *bitsize;
            if (!regnumText.empty())
            {
                const auto regnum = parseU32(regnumText);
                if (!regnum)
                {
                    continue;
                }
                info.number = *regnum;
            }
            else
            {
                info.number = nextNumber;
            }
            nextNumber  = info.number + 1;
            info.offset = offset;
            offset += (*bitsize + 7) / 8;
            description.registers.push_back(std::move(info));
        }
        description.xml = xml;
        return description;
    }

    std::optional<RspRegisterInfo> findRspRegister(const RspTargetDescription& description, std::string_view name)
    {
        const auto found = std::find_if(description.registers.begin(),
                                        description.registers.end(),
                                        [name](const RspRegisterInfo& info)
                                        {
                                            return info.name == name;
                                        });
        if (found == description.registers.end())
        {
            return std::nullopt;
        }
        return *found;
    }

    std::optional<RspRegisterInfo> fallbackRspRegister(std::string_view name)
    {
        const auto& table = fallbackTable();
        const auto  found = std::find_if(table.begin(),
                                         table.end(),
                                         [name](const RspRegisterInfo& info)
                                         {
                                            return info.name == name;
                                         });
        if (found == table.end())
        {
            return std::nullopt;
        }
        return *found;
    }

    std::optional<std::uint64_t> readRspRegisterValue(std::span<const std::byte> registers, const RspRegisterInfo& info)
    {
        const auto bytes = static_cast<std::size_t>((info.bitsize + 7) / 8);
        if (bytes == 0 || bytes > sizeof(std::uint64_t) || info.offset + bytes > registers.size())
        {
            return std::nullopt;
        }

        std::uint64_t value = 0;
        for (std::size_t index = 0; index < bytes; ++index)
        {
            value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(registers[info.offset + index]))
                  << (index * 8);
        }
        return value;
    }

    RspClient::RspClient(std::unique_ptr<RspTransport> transport) : _transport {std::move(transport)} {}

    RspClient::~RspClient()
    {
        if (_transport)
        {
            _transport->close();
        }
    }

    bool RspClient::connect(const RspEndpoint& endpoint, std::chrono::milliseconds timeout, std::string& error)
    {
        if (!_transport)
        {
            error = "the RSP client has no transport";
            return false;
        }
        return _transport->open(endpoint, timeout, error);
    }

    bool RspClient::negotiate(std::chrono::milliseconds timeout, std::string& error)
    {
        std::string reply;
        if (!sendPacket("qSupported:multiprocess+;xmlRegisters=i386", reply, timeout, error))
        {
            return false;
        }
        _supported = reply;
        if (reply.find("QStartNoAckMode+") == std::string::npos)
        {
            return true;
        }
        if (!sendPacket("QStartNoAckMode", reply, timeout, error))
        {
            return false;
        }
        if (reply == "OK")
        {
            _noAck = true;
        }
        return true;
    }

    bool RspClient::readTargetDescription(RspTargetDescription&     description,
                                          std::chrono::milliseconds timeout,
                                          std::string&              error)
    {
        const auto               deadline = Clock::now() + timeout;
        std::vector<std::string> pending {"target.xml"};
        std::vector<std::string> seen;
        std::string              combined;
        std::size_t              files = 0;

        while (!pending.empty())
        {
            const auto annex = pending.front();
            pending.erase(pending.begin());
            if (std::find(seen.begin(), seen.end(), annex) != seen.end())
            {
                continue;
            }
            seen.push_back(annex);
            if (++files > kMaxRspFiles)
            {
                error = "the stub advertised too many target description files";
                return false;
            }

            std::string content;
            if (!fetchFile(annex, content, deadline, error))
            {
                return false;
            }
            combined += content;
            if (combined.size() > kMaxRspFileBytes)
            {
                error = "the target description is too large";
                return false;
            }

            static const std::regex hrefPattern("href=\"([^\"]+)\"");
            for (std::sregex_iterator it(content.begin(), content.end(), hrefPattern), end; it != end; ++it)
            {
                const auto href = (*it)[1].str();
                if (std::find(seen.begin(), seen.end(), href) == seen.end())
                {
                    pending.push_back(href);
                }
            }
        }

        description = parseRspTargetDescription(combined);
        if (description.registers.empty())
        {
            error = "the stub advertised no registers";
            return false;
        }
        return true;
    }

    bool
    RspClient::queryThreads(std::vector<std::string>& threads, std::chrono::milliseconds timeout, std::string& error)
    {
        std::string reply;
        if (!sendPacket("qfThreadInfo", reply, timeout, error))
        {
            return false;
        }
        while (true)
        {
            if (reply.empty() || (reply[0] != 'm' && reply[0] != 'l'))
            {
                error = "the stub returned a malformed thread list";
                return false;
            }
            const auto ids = std::string_view {reply}.substr(1);
            if (!ids.empty() && ids != "0")
            {
                std::size_t start = 0;
                while (start <= ids.size())
                {
                    const auto comma = ids.find(',', start);
                    const auto end   = comma == std::string_view::npos ? ids.size() : comma;
                    if (end > start)
                    {
                        threads.emplace_back(ids.substr(start, end - start));
                        if (threads.size() > kMaxRspThreads)
                        {
                            error = "the stub reported too many threads";
                            return false;
                        }
                    }
                    if (comma == std::string_view::npos)
                    {
                        break;
                    }
                    start = comma + 1;
                }
            }
            if (reply[0] == 'l')
            {
                return true;
            }
            if (!sendPacket("qsThreadInfo", reply, timeout, error))
            {
                return false;
            }
        }
    }

    bool RspClient::selectThread(std::string_view thread, std::chrono::milliseconds timeout, std::string& error)
    {
        std::string command {"Hg"};
        command += thread;
        std::string reply;
        if (!sendPacket(command, reply, timeout, error))
        {
            return false;
        }
        if (reply != "OK")
        {
            error = "the stub rejected thread " + std::string {thread};
            return false;
        }
        return true;
    }

    bool RspClient::readAllRegisters(std::vector<std::byte>&   registers,
                                     std::chrono::milliseconds timeout,
                                     std::string&              error)
    {
        std::string reply;
        if (!sendPacket("g", reply, timeout, error))
        {
            return false;
        }
        if (!reply.empty() && reply[0] == 'E')
        {
            error = "the stub could not read the registers";
            return false;
        }
        auto decoded = decodeHex(reply);
        if (!decoded)
        {
            error = "the stub returned malformed register bytes";
            return false;
        }
        registers = std::move(*decoded);
        return true;
    }

    bool RspClient::readRegister(std::uint32_t             number,
                                 std::uint64_t&            value,
                                 std::chrono::milliseconds timeout,
                                 std::string&              error)
    {
        std::string           command {"p"};
        static constexpr char digits[] = "0123456789abcdef";
        bool                  started  = false;
        for (int shift = 28; shift >= 0; shift -= 4)
        {
            const auto digit = (number >> shift) & 0x0F;
            if (digit != 0)
            {
                started = true;
            }
            if (started)
            {
                command.push_back(digits[digit]);
            }
        }
        if (!started)
        {
            command.push_back('0');
        }

        std::string reply;
        if (!sendPacket(command, reply, timeout, error))
        {
            return false;
        }
        if (!reply.empty() && reply[0] == 'E')
        {
            error = "the stub could not read register " + std::to_string(number);
            return false;
        }
        auto decoded = decodeHex(reply);
        if (!decoded || decoded->size() > sizeof(std::uint64_t))
        {
            error = "the stub returned a malformed register value";
            return false;
        }

        std::uint64_t result = 0;
        for (std::size_t index = 0; index < decoded->size(); ++index)
        {
            result |= static_cast<std::uint64_t>(std::to_integer<unsigned char>((*decoded)[index])) << (index * 8);
        }
        value = result;
        return true;
    }

    bool RspClient::insertWatchpoint(std::uint64_t             address,
                                     std::uint32_t             size,
                                     std::chrono::milliseconds timeout,
                                     std::string&              error)
    {
        static constexpr char digits[] = "0123456789abcdef";
        std::string           command {"Z2,"};
        bool                  started = false;
        for (int shift = 60; shift >= 0; shift -= 4)
        {
            const auto digit = (address >> shift) & 0x0F;
            if (digit != 0)
            {
                started = true;
            }
            if (started)
            {
                command.push_back(digits[digit]);
            }
        }
        if (!started)
        {
            command.push_back('0');
        }
        command.push_back(',');
        command += std::to_string(size);
        return checkAccepted(command, timeout, error);
    }

    bool RspClient::removeWatchpoint(std::uint64_t             address,
                                     std::uint32_t             size,
                                     std::chrono::milliseconds timeout,
                                     std::string&              error)
    {
        static constexpr char digits[] = "0123456789abcdef";
        std::string           command {"z2,"};
        bool                  started = false;
        for (int shift = 60; shift >= 0; shift -= 4)
        {
            const auto digit = (address >> shift) & 0x0F;
            if (digit != 0)
            {
                started = true;
            }
            if (started)
            {
                command.push_back(digits[digit]);
            }
        }
        if (!started)
        {
            command.push_back('0');
        }
        command.push_back(',');
        command += std::to_string(size);
        return checkAccepted(command, timeout, error);
    }

    bool RspClient::continueExecution(std::chrono::milliseconds timeout, std::string& error)
    {
        const auto command = _supported.find("vCont;c") != std::string::npos ? "vCont;c" : "c";
        return writePacketOnly(command, timeout, error);
    }

    RspPollResult RspClient::pollPacket(std::string& packet, std::chrono::milliseconds timeout, std::string& error)
    {
        const auto deadline = Clock::now() + timeout;
        while (true)
        {
            while (!_buffer.empty() && (_buffer.front() == '+' || _buffer.front() == '-'))
            {
                _buffer.erase(0, 1);
            }

            const auto start = _buffer.find('$');
            if (start != std::string::npos)
            {
                if (start != 0)
                {
                    _buffer.erase(0, start);
                }
                const auto end = _buffer.find('#', 1);
                if (end != std::string::npos && _buffer.size() >= end + 3)
                {
                    const int high = hexDigit(_buffer[end + 1]);
                    const int low  = hexDigit(_buffer[end + 2]);
                    if (high < 0 || low < 0)
                    {
                        error = "the stub sent a malformed checksum";
                        return RspPollResult::Closed;
                    }
                    unsigned expected = 0;
                    for (std::size_t index = 1; index < end; ++index)
                    {
                        expected += static_cast<unsigned char>(_buffer[index]);
                    }
                    if (((high << 4) | low) != static_cast<int>(expected & 0xFF))
                    {
                        error = "the stub sent a bad checksum";
                        return RspPollResult::Closed;
                    }
                    packet = _buffer.substr(1, end - 1);
                    _buffer.erase(0, end + 3);
                    if (!_noAck)
                    {
                        const std::array<std::byte, 1> accept {std::byte {'+'}};
                        const auto                     left = remaining(deadline);
                        if (!_transport->writeAll(accept, left, error))
                        {
                            return RspPollResult::Closed;
                        }
                    }
                    return RspPollResult::Packet;
                }
            }
            else if (!_buffer.empty())
            {
                error = "the stub sent an unexpected byte";
                return RspPollResult::Closed;
            }

            std::array<std::byte, 4096> chunk {};
            const auto                  left = remaining(deadline);
            if (left == std::chrono::milliseconds::zero())
            {
                return RspPollResult::Timeout;
            }
            const auto result = _transport->readSome(chunk, left, error);
            if (result < 0)
            {
                if (error.find("timed out") != std::string::npos)
                {
                    error.clear();
                    return RspPollResult::Timeout;
                }
                return RspPollResult::Closed;
            }
            if (result == 0)
            {
                if (error.empty())
                {
                    error = "the stub closed the connection";
                }
                return RspPollResult::Closed;
            }
            _buffer.append(reinterpret_cast<const char*>(chunk.data()), static_cast<std::size_t>(result));
        }
    }

    void RspClient::drainPending()
    {
        std::string ignored;
        for (int packet = 0; packet < 16; ++packet)
        {
            const auto drained = pollPacket(ignored, std::chrono::milliseconds {10}, ignored);
            if (drained != RspPollResult::Packet)
            {
                return;
            }
            ignored.clear();
        }
    }

    bool RspClient::writePacketOnly(std::string_view data, std::chrono::milliseconds timeout, std::string& error)
    {
        const auto packet   = buildPacket(data);
        const auto deadline = Clock::now() + timeout;
        return writeRaw(std::as_bytes(std::span {packet}), deadline, error);
    }

    bool RspClient::checkAccepted(std::string_view command, std::chrono::milliseconds timeout, std::string& error)
    {
        std::string reply;
        if (!sendPacket(command, reply, timeout, error))
        {
            return false;
        }
        if (reply != "OK")
        {
            error = "the stub rejected " + std::string {command};
            return false;
        }
        return true;
    }

    bool RspClient::noAck() const noexcept
    {
        return _noAck;
    }

    const std::string& RspClient::supported() const noexcept
    {
        return _supported;
    }

    bool RspClient::sendPacket(std::string_view          data,
                               std::string&              reply,
                               std::chrono::milliseconds timeout,
                               std::string&              error)
    {
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            const std::string packet = buildPacket(data);

            const auto deadline = Clock::now() + timeout;
            if (!writeRaw(std::as_bytes(std::span {packet}), deadline, error))
            {
                return false;
            }

            unsigned char first = 0;
            bool          retry = false;
            while (true)
            {
                if (!readByte(deadline, first, error))
                {
                    return false;
                }
                if (first == '+')
                {
                    continue;
                }
                if (first == '-')
                {
                    retry = true;
                    break;
                }
                break;
            }
            if (retry)
            {
                continue;
            }
            if (first != '$')
            {
                error = "the stub sent an unexpected byte";
                return false;
            }

            std::string body;
            while (true)
            {
                unsigned char byte = 0;
                if (!readByte(deadline, byte, error))
                {
                    return false;
                }
                if (byte == '#')
                {
                    break;
                }
                body.push_back(static_cast<char>(byte));
            }

            unsigned char high = 0;
            unsigned char low  = 0;
            if (!readByte(deadline, high, error) || !readByte(deadline, low, error))
            {
                return false;
            }
            const int highDigit = hexDigit(static_cast<char>(high));
            const int lowDigit  = hexDigit(static_cast<char>(low));
            if (highDigit < 0 || lowDigit < 0)
            {
                error = "the stub sent a malformed checksum";
                return false;
            }

            unsigned expected = 0;
            for (const char value : body)
            {
                expected += static_cast<unsigned char>(value);
            }
            if (((highDigit << 4) | lowDigit) != static_cast<int>(expected & 0xFF))
            {
                if (!_noAck)
                {
                    const std::array<std::byte, 1> reject {static_cast<std::byte>('-')};
                    std::string                    ignored;
                    if (!writeRaw(reject, deadline, ignored))
                    {
                        error = "the stub sent a bad checksum";
                        return false;
                    }
                }
                error = "the stub sent a bad checksum";
                return false;
            }

            if (!_noAck)
            {
                const std::array<std::byte, 1> accept {std::byte {'+'}};
                if (!writeRaw(accept, deadline, error))
                {
                    return false;
                }
            }
            reply = std::move(body);
            return true;
        }

        error = "the stub rejected the packet three times";
        return false;
    }

    bool RspClient::readByte(std::chrono::steady_clock::time_point deadline, unsigned char& value, std::string& error)
    {
        if (_buffer.empty())
        {
            std::array<std::byte, 4096> chunk {};
            const auto                  left   = remaining(deadline);
            const auto                  result = _transport->readSome(chunk, left, error);
            if (result < 0)
            {
                return false;
            }
            if (result == 0)
            {
                error = "the stub closed the connection";
                return false;
            }
            _buffer.append(reinterpret_cast<const char*>(chunk.data()), static_cast<std::size_t>(result));
        }
        value = static_cast<unsigned char>(_buffer.front());
        _buffer.erase(0, 1);
        return true;
    }

    bool RspClient::writeRaw(std::span<const std::byte>            data,
                             std::chrono::steady_clock::time_point deadline,
                             std::string&                          error)
    {
        return _transport->writeAll(data, remaining(deadline), error);
    }

    bool RspClient::fetchFile(std::string_view                      annex,
                              std::string&                          content,
                              std::chrono::steady_clock::time_point deadline,
                              std::string&                          error)
    {
        std::size_t offset = 0;
        while (true)
        {
            std::string command {"qXfer:features:read:"};
            command += annex;
            command.push_back(':');
            static constexpr char digits[] = "0123456789abcdef";
            bool                  started  = false;
            for (int shift = (sizeof(offset) * 8) - 4; shift >= 0; shift -= 4)
            {
                const auto digit = (offset >> shift) & 0x0F;
                if (digit != 0)
                {
                    started = true;
                }
                if (started)
                {
                    command.push_back(digits[digit]);
                }
            }
            if (!started)
            {
                command.push_back('0');
            }
            command += ",fff";

            std::string reply;
            if (!sendPacket(command, reply, remaining(deadline), error))
            {
                return false;
            }
            if (reply.size() < 2 || (reply[0] != 'l' && reply[0] != 'm'))
            {
                error = "the stub could not read " + std::string {annex};
                return false;
            }
            content.append(reply, 1, std::string::npos);
            if (content.size() > kMaxRspFileBytes)
            {
                error = "the target description is too large";
                return false;
            }
            offset += reply.size() - 1;
            if (reply[0] == 'l')
            {
                return true;
            }
            if (reply.size() == 1)
            {
                error = "the stub stopped in the middle of " + std::string {annex};
                return false;
            }
        }
    }
} // namespace mpfshost::debug
