#include <catch2/catch.hpp>

#include "core/debug/debug_controller.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using namespace std::chrono_literals;
    using mpfshost::debug::DebugController;
    using mpfshost::debug::QmpTransport;
    using mpfshost::debug::RspEndpoint;
    using mpfshost::debug::RspTransport;

    std::string rspPacket(std::string_view body)
    {
        static constexpr char digits[] = "0123456789abcdef";
        unsigned              sum      = 0;
        for (const char value : body)
        {
            sum += static_cast<unsigned char>(value);
        }
        std::string packet {"$"};
        packet += body;
        packet.push_back('#');
        packet.push_back(digits[(sum >> 4) & 0x0F]);
        packet.push_back(digits[sum & 0x0F]);
        return packet;
    }

    struct FakeQmpState
    {
        std::deque<std::string>  incoming;
        std::vector<std::string> written;
        bool                     running {false};
        bool                     stubActive {false};
        bool                     startFails {false};

        FakeQmpState()
        {
            incoming.emplace_back(R"({"QMP":{"version":{"qemu":{"major":10,"minor":2,"micro":2}}}})");
        }

        [[nodiscard]] std::size_t count(std::string_view needle) const
        {
            return static_cast<std::size_t>(std::count_if(written.begin(),
                                                          written.end(),
                                                          [needle](const std::string& line)
                                                          {
                                                              return line.find(needle) != std::string::npos;
                                                          }));
        }
    };

    class FakeQmpTransport : public QmpTransport
    {
    public:
        explicit FakeQmpTransport(std::shared_ptr<FakeQmpState> state) : _state {std::move(state)} {}

        [[nodiscard]] bool open(const std::filesystem::path&, std::chrono::milliseconds, std::string&) override
        {
            return true;
        }

        [[nodiscard]] bool writeLine(std::string_view line, std::chrono::milliseconds, std::string&) override
        {
            _state->written.emplace_back(line);
            const std::string text {line};

            if (text.find("qmp_capabilities") != std::string::npos)
            {
                _state->incoming.emplace_back(R"({"return":{}})");
            }
            else if (text.find("query-status") != std::string::npos)
            {
                _state->incoming.emplace_back(_state->running ? R"({"return":{"status":"running","running":true}})"
                                                              : R"({"return":{"status":"paused","running":false}})");
            }
            else if (text.find("query-cpus-fast") != std::string::npos)
            {
                _state->incoming.emplace_back(R"({"return":[{},{"cpu-index":1}]})");
            }
            else if (text.find("info chardev") != std::string::npos)
            {
                _state->incoming.emplace_back(
                    _state->stubActive
                        ? R"({"return":"gdb: filename=disconnected:unix:/tmp/x\r\n#chr085: filename=gdb\r\n"})"
                        : R"({"return":"#chr085: filename=gdb\r\n"})");
            }
            else if (text.find("gdbserver unix:") != std::string::npos)
            {
                if (_state->startFails)
                {
                    _state->incoming.emplace_back(R"({"return":"gdbstub: couldn't create chardev\r\n"})");
                }
                else
                {
                    _state->stubActive = true;
                    _state->incoming.emplace_back(
                        R"({"return":"Waiting for gdb connection on device 'unix:/tmp/x'\r\n"})");
                }
            }
            else if (text.find("gdbserver tcp:") != std::string::npos)
            {
                _state->stubActive = true;
                _state->incoming.emplace_back(
                    R"({"return":"Waiting for gdb connection on device 'tcp:127.0.0.1:1'\r\n"})");
            }
            else if (text.find("gdbserver none") != std::string::npos)
            {
                _state->stubActive = false;
                _state->incoming.emplace_back(R"({"return":"Disabled gdbserver\r\n"})");
            }
            else
            {
                _state->incoming.emplace_back(R"({"return":{}})");
            }
            return true;
        }

        [[nodiscard]] bool
        readLine(std::string& line, std::chrono::milliseconds, bool& closed, std::string& error) override
        {
            if (_state->incoming.empty())
            {
                closed = true;
                error  = "the fake QMP has no reply";
                return false;
            }
            line = std::move(_state->incoming.front());
            _state->incoming.pop_front();
            return true;
        }

        void close() noexcept override {}

    private:
        std::shared_ptr<FakeQmpState> _state;
    };

    class FakeRspTransport : public RspTransport
    {
    public:
        std::string pending;
        std::string registersXml;
        std::string registerBytes;
        bool        noAck {false};
        bool        fail {false};

        [[nodiscard]] bool open(const RspEndpoint&, std::chrono::milliseconds, std::string&) override
        {
            return !fail;
        }

        [[nodiscard]] std::ptrdiff_t
        readSome(std::span<std::byte> buffer, std::chrono::milliseconds, std::string& error) override
        {
            if (fail || pending.empty())
            {
                error = "the fake RSP has no data";
                return -1;
            }
            const auto size = std::min(buffer.size(), pending.size());
            std::memcpy(buffer.data(), pending.data(), size);
            pending.erase(0, size);
            return static_cast<std::ptrdiff_t>(size);
        }

        [[nodiscard]] bool writeAll(std::span<const std::byte> data, std::chrono::milliseconds, std::string&) override
        {
            const std::string packet {reinterpret_cast<const char*>(data.data()), data.size()};
            const auto        start = packet.find('$');
            const auto        end   = packet.find('#', start + 1);
            if (start == std::string::npos || end == std::string::npos)
            {
                return true;
            }
            const auto body = packet.substr(start + 1, end - start - 1);
            respond(body);
            return true;
        }

        void close() noexcept override {}

        void respond(std::string_view body)
        {
            const auto quote = [this](std::string_view reply)
            {
                pending += rspPacket(reply);
            };
            const auto quoteAck = [this, &quote](std::string_view reply)
            {
                pending += "+";
                quote(reply);
            };

            if (body.starts_with("qSupported"))
            {
                quoteAck("PacketSize=1000;QStartNoAckMode+");
            }
            else if (body == "QStartNoAckMode")
            {
                quoteAck("OK");
                noAck = true;
            }
            else if (body.starts_with("qXfer:features:read:target.xml:"))
            {
                quote("l" + registersXml);
            }
            else if (body == "qfThreadInfo")
            {
                quoteAck("m1.1");
            }
            else if (body == "qsThreadInfo")
            {
                quoteAck("l");
            }
            else if (body == "Hg1.1")
            {
                quoteAck("OK");
            }
            else if (body == "g")
            {
                quoteAck(registerBytes);
            }
            else if (body.starts_with("p"))
            {
                quoteAck("0000000000000000");
            }
            else
            {
                quoteAck("OK");
            }
        }

        std::size_t acknowledgements {0};
    };

    std::string x86RegisterXml()
    {
        std::string                  xml {"<feature>"};
        static constexpr const char* names[] = {
            "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp",    "rsp", "r8",  "r9",  "r10",
            "r11", "r12", "r13", "r14", "r15", "rip", "eflags", "cr0", "cr3", "cr4", "cr8",
        };
        for (const auto* name : names)
        {
            xml += "<reg name=\"";
            xml += name;
            xml += "\" bitsize=\"";
            xml += std::string {name} == "eflags" ? "32" : "64";
            xml += "\"/>";
        }
        xml += "</feature>";
        return xml;
    }

    std::string registerBytes()
    {
        std::string bytes(172, '\0');
        const auto  put = [&bytes](std::size_t offset, std::uint64_t value)
        {
            for (std::size_t index = 0; index < 8 && offset + index < bytes.size(); ++index)
            {
                bytes[offset + index] = static_cast<char>((value >> (index * 8)) & 0xFF);
            }
        };
        put(0, 0x1122334455667788ull);   // rax
        put(128, 0xffffffffae89ab1bull); // rip
        put(140, 0x2468ace0ull);         // cr0
        put(148, 0x1a2b3000ull);         // cr3
        put(156, 0x0000f00eull);         // cr4
        put(164, 0x2ull);                // cr8

        static constexpr char digits[] = "0123456789abcdef";
        std::string           hex;
        hex.reserve(bytes.size() * 2);
        for (const char value : bytes)
        {
            const auto byte = static_cast<unsigned char>(value);
            hex.push_back(digits[byte >> 4]);
            hex.push_back(digits[byte & 0x0F]);
        }
        return hex;
    }
} // namespace

TEST_CASE("the debug controller starts and tears down a stub for register reads", "[core][debug][controller]")
{
    auto  qmp          = std::make_shared<FakeQmpState>();
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->registerBytes = registerBytes();

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [transport = std::move(qmpTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                [transport = std::move(rspTransport)]() mutable
                                {
                                    return std::move(transport);
                                }};

    bool        invalidState = false;
    std::string error;
    const auto  records = controller.registers({}, invalidState, error);
    REQUIRE(records.has_value());
    REQUIRE_FALSE(invalidState);
    REQUIRE(records->size() == 1);
    REQUIRE((*records)[0].threadId == "1.1");
    REQUIRE((*records)[0].rax == 0x1122334455667788ull);
    REQUIRE((*records)[0].rip == 0xffffffffae89ab1bull);
    REQUIRE((*records)[0].cr3 == 0x1a2b3000ull);
    REQUIRE((*records)[0].cr4 == 0x0000f00eull);
    REQUIRE((*records)[0].cr8 == 0x2ull);

    REQUIRE(qmp->count("gdbserver unix:") == 1);
    REQUIRE(qmp->count("gdbserver none") == 1);
    REQUIRE_FALSE(qmp->stubActive);
}

TEST_CASE("the debug controller refuses registers while the guest runs", "[core][debug][controller]")
{
    auto qmp          = std::make_shared<FakeQmpState>();
    auto qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    qmp->running      = true;
    auto rspTransport = std::make_unique<FakeRspTransport>();

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [transport = std::move(qmpTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                [transport = std::move(rspTransport)]() mutable
                                {
                                    return std::move(transport);
                                }};

    bool        invalidState = false;
    std::string error;
    const auto  records = controller.registers({}, invalidState, error);
    REQUIRE_FALSE(records.has_value());
    REQUIRE(invalidState);
    REQUIRE(error.find("paused") != std::string::npos);
    REQUIRE(qmp->count("gdbserver") == 0);
}

TEST_CASE("the debug controller refuses a stub owned by someone else", "[core][debug][controller]")
{
    auto qmp          = std::make_shared<FakeQmpState>();
    auto qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    qmp->stubActive   = true;
    auto rspTransport = std::make_unique<FakeRspTransport>();

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [transport = std::move(qmpTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                [transport = std::move(rspTransport)]() mutable
                                {
                                    return std::move(transport);
                                }};

    bool        invalidState = false;
    std::string error;
    const auto  records = controller.registers({}, invalidState, error);
    REQUIRE_FALSE(records.has_value());
    REQUIRE(invalidState);
    REQUIRE(error.find("another GDB stub") != std::string::npos);
    REQUIRE(qmp->count("gdbserver unix:") == 0);
}

TEST_CASE("the debug controller tears the stub down after a read failure", "[core][debug][controller]")
{
    auto  qmp          = std::make_shared<FakeQmpState>();
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = "<feature></feature>";

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [transport = std::move(qmpTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                [transport = std::move(rspTransport)]() mutable
                                {
                                    return std::move(transport);
                                }};

    bool        invalidState = false;
    std::string error;
    const auto  records = controller.registers({}, invalidState, error);
    REQUIRE_FALSE(records.has_value());
    REQUIRE_FALSE(invalidState);
    REQUIRE(qmp->count("gdbserver unix:") == 1);
    REQUIRE(qmp->count("gdbserver none") == 1);
}
