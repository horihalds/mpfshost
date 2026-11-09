#include <catch2/catch.hpp>

#include "core/debug/debug_controller.hpp"
#include "core/debug/debug_inspector.hpp"
#include "core/server/request_deadline.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
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
        std::deque<std::string>   incoming;
        std::vector<std::string>  written;
        bool                      running {false};
        bool                      stubActive {false};
        bool                      startFails {false};
        bool                      openFails {false};
        bool                      closed {false};
        bool                      stall {false};
        std::string               failCommand;
        std::size_t               opens {0};
        std::chrono::milliseconds lastReadTimeout {0};

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
            ++_state->opens;
            return !_state->openFails;
        }

        [[nodiscard]] bool writeLine(std::string_view line, std::chrono::milliseconds, std::string&) override
        {
            _state->written.emplace_back(line);
            if (_state->closed)
            {
                return true;
            }
            const std::string text {line};

            if (!_state->failCommand.empty() && text.find(_state->failCommand) != std::string::npos)
            {
                _state->incoming.emplace_back(R"({"error":{"class":"GenericError","desc":"the fake command failed"}})");
            }
            else if (text.find("qmp_capabilities") != std::string::npos)
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
            else if (text.find(R"("execute":"stop")") != std::string::npos)
            {
                _state->running = false;
                _state->incoming.emplace_back(R"({"return":{}})");
            }
            else if (text.find(R"("execute":"cont")") != std::string::npos)
            {
                _state->running = true;
                _state->incoming.emplace_back(R"({"return":{}})");
            }
            else
            {
                _state->incoming.emplace_back(R"({"return":{}})");
            }
            return true;
        }

        [[nodiscard]] bool
        readLine(std::string& line, std::chrono::milliseconds timeout, bool& closed, std::string& error) override
        {
            _state->lastReadTimeout = timeout;
            if (_state->closed)
            {
                closed = true;
                error  = "the fake QMP connection closed";
                return false;
            }
            if (_state->stall)
            {
                std::this_thread::sleep_for(timeout);
                closed = false;
                error  = "the fake QMP socket timed out";
                return false;
            }
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
        struct StopReply
        {
            StopReply(std::string                  stopThread,
                      std::uint64_t                stopRip,
                      std::uint64_t                stopRsp,
                      std::uint64_t                stopCr3,
                      std::optional<std::uint64_t> stopWatch = std::nullopt)
                : thread {std::move(stopThread)}, rip {stopRip}, rsp {stopRsp}, cr3 {stopCr3}, watch {stopWatch}
            {
            }

            std::string                  thread;
            std::uint64_t                rip {};
            std::uint64_t                rsp {};
            std::uint64_t                cr3 {};
            std::optional<std::uint64_t> watch;
        };

        static std::string stopPacket(const StopReply& stop)
        {
            std::string packet = "T05thread:" + stop.thread + ";";
            if (stop.watch.has_value())
            {
                std::array<char, 20> buffer {};
                const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), *stop.watch, 16);
                packet += "watch:";
                packet.append(buffer.data(), result.ptr);
                packet.push_back(';');
            }
            return packet;
        }

        std::string              pending;
        std::string              registersXml;
        std::string              registerBytes;
        bool                     noAck {false};
        bool                     fail {false};
        std::deque<StopReply>    stops;
        std::vector<std::string> threads {"1.1"};
        std::uint64_t            currentRip {};
        std::uint64_t            currentRsp {};
        std::uint64_t            currentCr3 {};
        std::size_t              watchInserts {0};
        std::size_t              watchRemoves {0};
        std::size_t              breakpointInserts {0};
        std::size_t              breakpointRemoves {0};
        std::size_t              continues {0};
        std::size_t              steps {0};

        [[nodiscard]] bool open(const RspEndpoint&, std::chrono::milliseconds, std::string&) override
        {
            return !fail;
        }

        [[nodiscard]] std::ptrdiff_t
        readSome(std::span<std::byte> buffer, std::chrono::milliseconds, std::string& error) override
        {
            if (fail)
            {
                error = "the fake RSP has no data";
                return -1;
            }
            if (pending.empty())
            {
                error = "the GDB stub socket timed out";
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
                std::string list {"m"};
                for (std::size_t index = 0; index < threads.size(); ++index)
                {
                    if (index != 0)
                    {
                        list.push_back(',');
                    }
                    list += threads[index];
                }
                quoteAck(list);
            }
            else if (body == "qsThreadInfo")
            {
                quoteAck("l");
            }
            else if (body.starts_with("Hg"))
            {
                const auto thread = body.substr(2);
                if (std::find(threads.begin(), threads.end(), thread) != threads.end())
                {
                    quoteAck("OK");
                }
                else
                {
                    quoteAck("E22");
                }
            }
            else if (body == "g")
            {
                quoteAck(registerBytes);
            }
            else if (body.starts_with("Z2,"))
            {
                ++watchInserts;
                quoteAck("OK");
            }
            else if (body.starts_with("z2,"))
            {
                ++watchRemoves;
                quoteAck("OK");
            }
            else if (body.starts_with("Z0,") || body.starts_with("Z1,"))
            {
                ++breakpointInserts;
                quoteAck("OK");
            }
            else if (body.starts_with("z0,") || body.starts_with("z1,"))
            {
                ++breakpointRemoves;
                quoteAck("OK");
            }
            else if (body == "vCont;c" || body == "c")
            {
                ++continues;
                if (!stops.empty())
                {
                    const auto stop = stops.front();
                    stops.pop_front();
                    currentRip = stop.rip;
                    currentRsp = stop.rsp;
                    currentCr3 = stop.cr3;
                    quoteAck(stopPacket(stop));
                }
            }
            else if (body.starts_with("vCont;s") || body == "s")
            {
                ++steps;
                if (!stops.empty())
                {
                    const auto stop = stops.front();
                    stops.pop_front();
                    currentRip = stop.rip;
                    currentRsp = stop.rsp;
                    currentCr3 = stop.cr3;
                    quoteAck(stopPacket(stop));
                }
            }
            else if (body.starts_with("p"))
            {
                std::uint32_t number = 0;
                std::from_chars(body.data() + 1, body.data() + body.size(), number, 16);
                std::uint64_t value = 0;
                if (number == 16)
                {
                    value = currentRip;
                }
                else if (number == 7)
                {
                    value = currentRsp;
                }
                else if (number == 19)
                {
                    value = currentCr3;
                }
                static constexpr char digits[] = "0123456789abcdef";
                std::string           hex;
                hex.reserve(16);
                for (int byteIndex = 0; byteIndex < 8; ++byteIndex)
                {
                    const auto byte = (value >> (byteIndex * 8)) & 0xFF;
                    hex.push_back(digits[(byte >> 4) & 0x0F]);
                    hex.push_back(digits[byte & 0x0F]);
                }
                quoteAck(hex);
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

    std::string registerBytes(std::uint64_t rip = 0xffffffffae89ab1bull)
    {
        std::string bytes(172, '\0');
        const auto  put = [&bytes](std::size_t offset, std::uint64_t value)
        {
            for (std::size_t index = 0; index < 8 && offset + index < bytes.size(); ++index)
            {
                bytes[offset + index] = static_cast<char>((value >> (index * 8)) & 0xFF);
            }
        };
        put(0, 0x1122334455667788ull); // rax
        put(128, rip);                 // rip
        put(140, 0x2468ace0ull);       // cr0
        put(148, 0x1a2b3000ull);       // cr3
        put(156, 0x0000f00eull);       // cr4
        put(164, 0x2ull);              // cr8

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

    class FakeInspector : public mpfshost::debug::DebugInspector
    {
    public:
        std::uint64_t                               dtb {0x1ae000};
        std::uint64_t                               dtbUser {0x200000};
        std::uint64_t                               watchAddress {0x5000};
        std::vector<std::uint64_t>                  values;
        std::size_t                                 valueIndex {0};
        mpfshost::debug::DebugModuleHit             module {0x7ff8451b0000, 0x100000, 0x7b1f1, "msvcrt.dll"};
        mpfshost::debug::DebugExportHit             exportHit {true, 0x7ff84522b180, 0x71, "memcpy"};
        std::optional<mpfshost::debug::DebugPdbHit> pdb {
            mpfshost::debug::DebugPdbHit {"memcpy", 4}
        };
        bool                  hasModule {true};
        std::function<void()> onWatchRead;

        [[nodiscard]] std::optional<mpfshost::debug::DebugProcessDtbs> processDtbs(std::uint32_t, std::string&) override
        {
            return mpfshost::debug::DebugProcessDtbs {dtb, dtbUser};
        }

        [[nodiscard]] std::optional<mpfshost::debug::DebugModuleHit>
        moduleForAddress(std::uint32_t, std::uint64_t address, std::string&) override
        {
            if (!hasModule || address < module.base || address >= module.base + module.imageSize)
            {
                return std::nullopt;
            }
            return module;
        }

        [[nodiscard]] mpfshost::debug::DebugExportHit
        nearestExport(std::uint32_t, const std::string&, std::uint64_t, std::string&) override
        {
            return exportHit;
        }

        [[nodiscard]] std::optional<mpfshost::debug::DebugPdbHit>
        pdbSymbol(std::uint32_t, const std::string&, std::uint64_t, std::string&) override
        {
            return pdb;
        }

        [[nodiscard]] bool readMemory(std::uint32_t,
                                      std::uint64_t           address,
                                      std::uint32_t           size,
                                      std::vector<std::byte>& out,
                                      std::string&            error) override
        {
            out.clear();
            if (address == watchAddress)
            {
                if (onWatchRead)
                {
                    auto callback = std::move(onWatchRead);
                    onWatchRead   = {};
                    callback();
                }
                if (valueIndex >= values.size())
                {
                    error = "the fake memory read failed";
                    return false;
                }
                const auto value = values[valueIndex++];
                for (std::uint32_t index = 0; index < size; ++index)
                {
                    out.push_back(static_cast<std::byte>((value >> (index * 8)) & 0xFF));
                }
                return true;
            }
            out.assign(size, std::byte {0x90});
            return true;
        }
    };
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
    bool        busy         = false;
    std::string error;
    bool        unavailable = false;
    const auto  records     = controller.registers({}, invalidState, busy, error, unavailable);
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
    bool        busy         = false;
    std::string error;
    bool        unavailable = false;
    const auto  records     = controller.registers({}, invalidState, busy, error, unavailable);
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
    bool        busy         = false;
    std::string error;
    bool        unavailable = false;
    const auto  records     = controller.registers({}, invalidState, busy, error, unavailable);
    REQUIRE_FALSE(records.has_value());
    REQUIRE(invalidState);
    REQUIRE(error.find("another GDB stub") != std::string::npos);
    REQUIRE(qmp->count("gdbserver unix:") == 0);
}

TEST_CASE("the debug controller reports a foreign stub as unowned", "[core][debug][controller]")
{
    auto qmp        = std::make_shared<FakeQmpState>();
    qmp->stubActive = true;
    qmp->running    = false;

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [qmp]
                                {
                                    return std::make_unique<FakeQmpTransport>(qmp);
                                },
                                []
                                {
                                    return std::make_unique<FakeRspTransport>();
                                }};

    bool        busy = false;
    std::string error;
    bool        unavailable = false;
    const auto  status      = controller.state(busy, error, unavailable);
    REQUIRE(status.has_value());
    REQUIRE(status->stubActive);
    REQUIRE_FALSE(status->stubOwned);
    REQUIRE(status->foreignStub);
}

TEST_CASE("the debug controller detaches a foreign stub on request", "[core][debug][controller]")
{
    auto qmp        = std::make_shared<FakeQmpState>();
    qmp->stubActive = true;
    qmp->running    = false;

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [qmp]
                                {
                                    return std::make_unique<FakeQmpTransport>(qmp);
                                },
                                []
                                {
                                    return std::make_unique<FakeRspTransport>();
                                }};

    bool        busy = false;
    std::string error;
    bool        unavailable = false;
    const auto  response    = controller.detach(busy, error, unavailable);
    REQUIRE(response.has_value());
    REQUIRE(response->stubStopped);
    REQUIRE(response->foreignStub);
    REQUIRE_FALSE(response->guestResumed);
    REQUIRE_FALSE(response->status.stubActive);
    REQUIRE(qmp->count("gdbserver none") == 1);
    REQUIRE_FALSE(qmp->stubActive);
}

TEST_CASE("detach resumes a guest the host paused", "[core][debug][controller]")
{
    auto qmp     = std::make_shared<FakeQmpState>();
    qmp->running = true;

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [qmp]
                                {
                                    return std::make_unique<FakeQmpTransport>(qmp);
                                },
                                []
                                {
                                    return std::make_unique<FakeRspTransport>();
                                }};

    bool        busy = false;
    std::string error;
    bool        unavailable = false;
    const auto  stopped     = controller.control(mpfshost::DebugControlAction::Stop, busy, error, unavailable);
    REQUIRE(stopped.has_value());
    REQUIRE_FALSE(qmp->running);

    const auto response = controller.detach(busy, error, unavailable);
    REQUIRE(response.has_value());
    REQUIRE(response->guestResumed);
    REQUIRE(response->status.runState == mpfshost::DebugRunState::Running);
    REQUIRE(qmp->running);
}

TEST_CASE("shutdown resumes a guest the host paused", "[core][debug][controller]")
{
    auto qmp     = std::make_shared<FakeQmpState>();
    qmp->running = true;

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [qmp]
                                {
                                    return std::make_unique<FakeQmpTransport>(qmp);
                                },
                                []
                                {
                                    return std::make_unique<FakeRspTransport>();
                                }};

    bool        busy = false;
    std::string error;
    bool        unavailable = false;
    const auto  stopped     = controller.control(mpfshost::DebugControlAction::Stop, busy, error, unavailable);
    REQUIRE(stopped.has_value());
    REQUIRE_FALSE(qmp->running);

    controller.shutdown();
    REQUIRE(qmp->running);
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
    bool        busy         = false;
    std::string error;
    bool        unavailable = false;
    const auto  records     = controller.registers({}, invalidState, busy, error, unavailable);
    REQUIRE_FALSE(records.has_value());
    REQUIRE_FALSE(invalidState);
    REQUIRE(qmp->count("gdbserver unix:") == 1);
    REQUIRE(qmp->count("gdbserver none") == 1);
}

TEST_CASE("the debug controller arms, resolves and tears down a write watchpoint", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->stops.push_back({"1.1", 0x7ff84522b1f1ull, 0x1234, 0x200123});

    FakeInspector inspector;
    inspector.values = {0xAA, 0xBB};

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [transport = std::move(qmpTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                [transport = std::move(rspTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                &inspector};

    std::vector<mpfshost::DebugWatchData> hits;
    bool                                  busy = false;
    std::string                           error;
    bool                                  unavailable = false;
    const auto                            end         = controller.watch(
        {42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 5000},
        [&hits](const mpfshost::DebugWatchData& hit)
        {
            hits.push_back(hit);
            return true;
        },
        {},
        busy,
        error,
        unavailable);
    REQUIRE(end.has_value());
    REQUIRE_FALSE(busy);
    REQUIRE(end->status == mpfshost::DebugWatchStatus::Completed);
    REQUIRE(end->hitCount == 1);
    REQUIRE(end->skippedCount == 0);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].rip == 0x7ff84522b1f1ull);
    REQUIRE(hits[0].rsp == 0x1234);
    REQUIRE(hits[0].cr3 == 0x200123);
    REQUIRE(hits[0].oldValue == 0xAA);
    REQUIRE(hits[0].newValue == 0xBB);
    REQUIRE(hits[0].module == "msvcrt.dll");
    REQUIRE(hits[0].moduleOffset == 0x7b1f1);
    REQUIRE(hits[0].exportFound);
    REQUIRE(hits[0].exportName == "memcpy");
    REQUIRE(hits[0].pdbFound);
    REQUIRE(hits[0].pdbSymbol == "memcpy");
    REQUIRE(hits[0].bytesBefore.size() == 16);
    REQUIRE(rsp->watchInserts == 1);
    REQUIRE(rsp->watchRemoves == 1);
    REQUIRE(rsp->continues == 1);
    REQUIRE_FALSE(qmp->stubActive);
    REQUIRE(qmp->running);
    REQUIRE(qmp->count("gdbserver none") == 1);
}

TEST_CASE("a watchpoint skips a hit whose CR3 belongs to another process", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->stops.push_back({"1.1", 0x7ff84522b1f1ull, 0x1234, 0x999123});
    rsp->stops.push_back({"1.1", 0x7ff84522b1f1ull, 0x1234, 0x200123});

    FakeInspector inspector;
    inspector.values = {0xAA, 0xAA, 0xBB};

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [transport = std::move(qmpTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                [transport = std::move(rspTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                &inspector};

    std::vector<mpfshost::DebugWatchData> hits;
    bool                                  busy = false;
    std::string                           error;
    bool                                  unavailable = false;
    const auto                            end         = controller.watch(
        {42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 5000},
        [&hits](const mpfshost::DebugWatchData& hit)
        {
            hits.push_back(hit);
            return true;
        },
        {},
        busy,
        error,
        unavailable);
    REQUIRE(end.has_value());
    REQUIRE(end->status == mpfshost::DebugWatchStatus::Completed);
    REQUIRE(end->hitCount == 1);
    REQUIRE(end->skippedCount == 1);
    REQUIRE(hits.size() == 1);
    REQUIRE(rsp->continues == 2);
    REQUIRE(qmp->running);
}

TEST_CASE("a watchpoint accepts a CR3 mismatch when the target memory changed", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->stops.push_back({"1.1", 0x7ff84522b1f1ull, 0x1234, 0x999123});

    FakeInspector inspector;
    inspector.values = {0xAA, 0xBB};

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [transport = std::move(qmpTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                [transport = std::move(rspTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                &inspector};

    std::vector<mpfshost::DebugWatchData> hits;
    bool                                  busy = false;
    std::string                           error;
    bool                                  unavailable = false;
    const auto                            end         = controller.watch(
        {42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 5000},
        [&hits](const mpfshost::DebugWatchData& hit)
        {
            hits.push_back(hit);
            return true;
        },
        {},
        busy,
        error,
        unavailable);
    REQUIRE(end.has_value());
    REQUIRE(end->status == mpfshost::DebugWatchStatus::Completed);
    REQUIRE(end->hitCount == 1);
    REQUIRE(end->skippedCount == 0);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].cr3 == 0x999123);
    REQUIRE(hits[0].oldValue == 0xAA);
    REQUIRE(hits[0].newValue == 0xBB);
    REQUIRE(rsp->continues == 1);
    REQUIRE(qmp->running);
}

TEST_CASE("a watchpoint accepts its quoted stop address without a CR3 match", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->stops.push_back({"1.1", 0xfffff800aa6ac7cfull, 0x1234, 0x999123, 0x5000});

    FakeInspector inspector;
    inspector.values = {0xAA, 0xBB};

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [transport = std::move(qmpTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                [transport = std::move(rspTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                &inspector};

    std::vector<mpfshost::DebugWatchData> hits;
    bool                                  busy = false;
    std::string                           error;
    bool                                  unavailable = false;
    const auto                            end         = controller.watch(
        {42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 5000},
        [&hits](const mpfshost::DebugWatchData& hit)
        {
            hits.push_back(hit);
            return true;
        },
        {},
        busy,
        error,
        unavailable);
    REQUIRE(end.has_value());
    REQUIRE(end->status == mpfshost::DebugWatchStatus::Completed);
    REQUIRE(end->hitCount == 1);
    REQUIRE(end->skippedCount == 0);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].oldValue == 0xAA);
    REQUIRE(hits[0].newValue == 0xBB);
    REQUIRE(rsp->continues == 1);
    REQUIRE(qmp->running);
}

TEST_CASE("a watchpoint skips a stop for another address", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->stops.push_back({"1.1", 0x7ff84522b1f1ull, 0x1234, 0x200123, 0x6000});

    FakeInspector inspector;
    inspector.values = {0xAA, 0xBB};

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [transport = std::move(qmpTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                [transport = std::move(rspTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                &inspector};

    std::vector<mpfshost::DebugWatchData> hits;
    bool                                  busy = false;
    std::string                           error;
    bool                                  unavailable = false;
    const auto                            end         = controller.watch(
        {42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 100},
        [&hits](const mpfshost::DebugWatchData& hit)
        {
            hits.push_back(hit);
            return true;
        },
        {},
        busy,
        error,
        unavailable);
    REQUIRE(end.has_value());
    REQUIRE(end->status == mpfshost::DebugWatchStatus::Timeout);
    REQUIRE(end->hitCount == 0);
    REQUIRE(end->skippedCount == 1);
    REQUIRE(hits.empty());
    REQUIRE(rsp->continues == 2);
    REQUIRE(qmp->running);
}

TEST_CASE("a watchpoint ignores a stale hit outside the target's modules", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->stops.push_back({"1.1", 0xfffff800aa6ac7cfull, 0x1234, 0x999123});

    FakeInspector inspector;
    inspector.values = {0xAA, 0xBB};

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [transport = std::move(qmpTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                [transport = std::move(rspTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                &inspector};

    std::vector<mpfshost::DebugWatchData> hits;
    bool                                  busy = false;
    std::string                           error;
    bool                                  unavailable = false;
    const auto                            end         = controller.watch(
        {42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 100},
        [&hits](const mpfshost::DebugWatchData& hit)
        {
            hits.push_back(hit);
            return true;
        },
        {},
        busy,
        error,
        unavailable);
    REQUIRE(end.has_value());
    REQUIRE(end->status == mpfshost::DebugWatchStatus::Timeout);
    REQUIRE(end->hitCount == 0);
    REQUIRE(end->skippedCount == 1);
    REQUIRE(hits.empty());
    REQUIRE(rsp->continues == 2);
    REQUIRE(qmp->running);
}

TEST_CASE("the watchpoint is armed once even with several vCPUs", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->threads       = {"1.1", "1.2"};
    rsp->stops.push_back({"1.2", 0x7ff84522b1f1ull, 0x1234, 0x200123});

    FakeInspector inspector;
    inspector.values = {0xAA, 0xBB};

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [transport = std::move(qmpTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                [transport = std::move(rspTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                &inspector};

    bool        busy = false;
    std::string error;
    bool        unavailable = false;
    const auto  end =
        controller.watch({42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 5000}, {}, {}, busy, error, unavailable);
    REQUIRE(end.has_value());
    REQUIRE(end->status == mpfshost::DebugWatchStatus::Completed);
    REQUIRE(rsp->watchInserts == 1);
    REQUIRE(rsp->watchRemoves == 1);
    REQUIRE(qmp->running);
}

TEST_CASE("a watchpoint times out and still restores the guest", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();

    FakeInspector inspector;
    inspector.values = {0xAA};

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [transport = std::move(qmpTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                [transport = std::move(rspTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                &inspector};

    bool        busy = false;
    std::string error;
    bool        unavailable = false;
    const auto  end =
        controller.watch({42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 5}, {}, {}, busy, error, unavailable);
    REQUIRE(end.has_value());
    REQUIRE(end->status == mpfshost::DebugWatchStatus::Timeout);
    REQUIRE(end->hitCount == 0);
    REQUIRE(rsp->watchRemoves == 1);
    REQUIRE_FALSE(qmp->stubActive);
    REQUIRE(qmp->running);
}

TEST_CASE("cancelling a watchpoint tears the stub down", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();

    FakeInspector inspector;
    inspector.values = {0xAA};

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [transport = std::move(qmpTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                [transport = std::move(rspTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                &inspector};

    bool        busy = false;
    std::string error;
    bool        unavailable = false;
    const auto  end         = controller.watch(
        {42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 5000},
        {},
        []
        {
            return true;
        },
        busy,
        error,
        unavailable);
    REQUIRE(end.has_value());
    REQUIRE(end->status == mpfshost::DebugWatchStatus::Cancelled);
    REQUIRE(rsp->watchRemoves == 1);
    REQUIRE_FALSE(qmp->stubActive);
    REQUIRE(qmp->running);
}

TEST_CASE("cancelling a watchpoint after its first hit stops the watch", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->stops.push_back({"1.1", 0x7ff84522b1f1ull, 0x1234, 0x200123});
    rsp->stops.push_back({"1.1", 0x7ff84522b1f1ull, 0x1234, 0x200123});

    FakeInspector inspector;
    inspector.values = {0xAA, 0xBB, 0xCC};

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [transport = std::move(qmpTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                [transport = std::move(rspTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                &inspector};

    auto        first = true;
    std::size_t hits  = 0;
    bool        busy  = false;
    std::string error;
    bool        unavailable = false;
    const auto  end         = controller.watch(
        {42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 2, 5000},
        [&hits](const mpfshost::DebugWatchData&)
        {
            ++hits;
            return true;
        },
        [&first]
        {
            const auto result = !first;
            first             = false;
            return result;
        },
        busy,
        error,
        unavailable);
    REQUIRE(end.has_value());
    REQUIRE(end->status == mpfshost::DebugWatchStatus::Cancelled);
    REQUIRE(end->hitCount == 1);
    REQUIRE(hits == 1);
    REQUIRE(rsp->watchRemoves == 1);
    REQUIRE_FALSE(qmp->stubActive);
    REQUIRE(qmp->running);
}

TEST_CASE("a memory read failure during a watchpoint ends it as an error", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->stops.push_back({"1.1", 0x7ff84522b1f1ull, 0x1234, 0x200123});

    FakeInspector inspector;
    inspector.values = {0xAA};

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [transport = std::move(qmpTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                [transport = std::move(rspTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                &inspector};

    bool        busy = false;
    std::string error;
    bool        unavailable = false;
    const auto  end =
        controller.watch({42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 5000}, {}, {}, busy, error, unavailable);
    REQUIRE(end.has_value());
    REQUIRE(end->status == mpfshost::DebugWatchStatus::Error);
    REQUIRE(end->error.find("fake memory read") != std::string::npos);
    REQUIRE(rsp->watchRemoves == 1);
    REQUIRE_FALSE(qmp->stubActive);
    REQUIRE(qmp->running);
}

TEST_CASE("the debug controller arms and tears down software and hardware breakpoints", "[core][debug][controller]")
{
    SECTION("software")
    {
        auto qmp           = std::make_shared<FakeQmpState>();
        qmp->running       = true;
        auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
        auto  rspTransport = std::make_unique<FakeRspTransport>();
        auto* rsp          = rspTransport.get();
        rsp->registersXml  = x86RegisterXml();
        rsp->stops.push_back({"1.1", 0x7ff84522b1f1ull, 0x1234, 0x200123});

        FakeInspector inspector;

        DebugController controller {"/tmp/qmp.sock",
                                    "/tmp/stub.sock",
                                    [transport = std::move(qmpTransport)]() mutable
                                    {
                                        return std::move(transport);
                                    },
                                    [transport = std::move(rspTransport)]() mutable
                                    {
                                        return std::move(transport);
                                    },
                                    &inspector};

        bool        invalidState = false;
        bool        busy         = false;
        std::string error;
        bool        unavailable = false;
        const auto  response =
            controller.breakpoint({42, 0x7ff84522b1f1ull, mpfshost::DebugBreakpointKind::Software, 5000},
                                  {},
                                  invalidState,
                                  busy,
                                  error,
                                  unavailable);
        REQUIRE(response.has_value());
        REQUIRE_FALSE(invalidState);
        REQUIRE_FALSE(busy);
        REQUIRE(response->status == mpfshost::DebugBreakpointStatus::Hit);
        REQUIRE(response->rip == 0x7ff84522b1f1ull);
        REQUIRE(response->rsp == 0x1234);
        REQUIRE(response->cr3 == 0x200123);
        REQUIRE(response->address == 0x7ff84522b1f1ull);
        REQUIRE(response->module == "msvcrt.dll");
        REQUIRE(response->exportName == "memcpy");
        REQUIRE(response->bytesAtRip.size() == mpfshost::kDebugBreakpointBytes);
        REQUIRE(rsp->breakpointInserts == 1);
        REQUIRE(rsp->breakpointRemoves == 1);
        REQUIRE_FALSE(qmp->stubActive);
        REQUIRE(qmp->running);
    }

    SECTION("hardware")
    {
        auto qmp           = std::make_shared<FakeQmpState>();
        qmp->running       = true;
        auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
        auto  rspTransport = std::make_unique<FakeRspTransport>();
        auto* rsp          = rspTransport.get();
        rsp->registersXml  = x86RegisterXml();
        rsp->stops.push_back({"1.1", 0x7ff84522b1f1ull, 0x1234, 0x200123});

        FakeInspector inspector;

        DebugController controller {"/tmp/qmp.sock",
                                    "/tmp/stub.sock",
                                    [transport = std::move(qmpTransport)]() mutable
                                    {
                                        return std::move(transport);
                                    },
                                    [transport = std::move(rspTransport)]() mutable
                                    {
                                        return std::move(transport);
                                    },
                                    &inspector};

        bool        invalidState = false;
        bool        busy         = false;
        std::string error;
        bool        unavailable = false;
        const auto  response =
            controller.breakpoint({42, 0x7ff84522b1f1ull, mpfshost::DebugBreakpointKind::Hardware, 5000},
                                  {},
                                  invalidState,
                                  busy,
                                  error,
                                  unavailable);
        REQUIRE(response.has_value());
        REQUIRE(response->status == mpfshost::DebugBreakpointStatus::Hit);
        REQUIRE(rsp->breakpointInserts == 1);
        REQUIRE(rsp->breakpointRemoves == 1);
        REQUIRE_FALSE(qmp->stubActive);
        REQUIRE(qmp->running);
    }
}

TEST_CASE("a breakpoint skips a stop that belongs to another process", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->stops.push_back({"1.1", 0xfffff800aa6ac7cfull, 0x1234, 0x999123});
    rsp->stops.push_back({"1.1", 0x7ff84522b1f1ull, 0x1234, 0x200123});

    FakeInspector inspector;

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [transport = std::move(qmpTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                [transport = std::move(rspTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                &inspector};

    bool        invalidState = false;
    bool        busy         = false;
    std::string error;
    bool        unavailable = false;
    const auto  response = controller.breakpoint({42, 0x7ff84522b1f1ull, mpfshost::DebugBreakpointKind::Hardware, 5000},
                                                 {},
                                                 invalidState,
                                                 busy,
                                                 error,
                                                 unavailable);
    REQUIRE(response.has_value());
    REQUIRE(response->status == mpfshost::DebugBreakpointStatus::Hit);
    REQUIRE(response->skippedCount == 1);
    REQUIRE(rsp->continues == 2);
    REQUIRE(qmp->running);
}

TEST_CASE("a breakpoint skips a matching CR3 stop away from the breakpoint address", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->stops.push_back({"1.1", 0x7ff84522b1f1ull, 0x1234, 0x200123});
    rsp->stops.push_back({"1.1", 0x7ff8451b1234ull, 0x1234, 0x200123});

    FakeInspector inspector;

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [transport = std::move(qmpTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                [transport = std::move(rspTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                &inspector};

    bool        invalidState = false;
    bool        busy         = false;
    std::string error;
    bool        unavailable = false;
    const auto  response = controller.breakpoint({42, 0x7ff8451b1234ull, mpfshost::DebugBreakpointKind::Hardware, 5000},
                                                 {},
                                                 invalidState,
                                                 busy,
                                                 error,
                                                 unavailable);
    REQUIRE(response.has_value());
    REQUIRE(response->status == mpfshost::DebugBreakpointStatus::Hit);
    REQUIRE(response->skippedCount == 1);
    REQUIRE(response->rip == 0x7ff8451b1234ull);
    REQUIRE(rsp->continues == 2);
}

TEST_CASE("a breakpoint times out and still restores the guest", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();

    FakeInspector inspector;

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [transport = std::move(qmpTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                [transport = std::move(rspTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                &inspector};

    bool        invalidState = false;
    bool        busy         = false;
    std::string error;
    bool        unavailable = false;
    const auto  response    = controller.breakpoint({42, 0x7ff84522b1f1ull, mpfshost::DebugBreakpointKind::Software, 5},
                                                    {},
                                                    invalidState,
                                                    busy,
                                                    error,
                                                    unavailable);
    REQUIRE(response.has_value());
    REQUIRE(response->status == mpfshost::DebugBreakpointStatus::Timeout);
    REQUIRE(rsp->breakpointRemoves == 1);
    REQUIRE_FALSE(qmp->stubActive);
    REQUIRE(qmp->running);
}

TEST_CASE("the debug controller single-steps a paused guest", "[core][debug][controller]")
{
    auto  qmp          = std::make_shared<FakeQmpState>();
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->registerBytes = registerBytes(0x7ff84522b1f1ull);
    rsp->stops.push_back({"1.1", 0x7ff84522b1f1ull, 0x1234, 0x200123});

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
    bool        busy         = false;
    std::string error;
    bool        unavailable = false;
    const auto  response    = controller.step({0}, invalidState, busy, error, unavailable);
    REQUIRE(response.has_value());
    REQUIRE_FALSE(invalidState);
    REQUIRE_FALSE(busy);
    REQUIRE(response->record.vcpu == 0);
    REQUIRE(response->record.threadId == "1.1");
    REQUIRE(response->record.rip == 0x7ff84522b1f1ull);
    REQUIRE(rsp->steps == 1);
    REQUIRE(qmp->count("gdbserver none") == 1);
    REQUIRE_FALSE(qmp->stubActive);
    REQUIRE_FALSE(qmp->running);
}

TEST_CASE("a single step refuses a running guest", "[core][debug][controller]")
{
    auto qmp          = std::make_shared<FakeQmpState>();
    qmp->running      = true;
    auto qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
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
    bool        busy         = false;
    std::string error;
    bool        unavailable = false;
    const auto  response    = controller.step({0}, invalidState, busy, error, unavailable);
    REQUIRE_FALSE(response.has_value());
    REQUIRE(invalidState);
    REQUIRE(error.find("paused") != std::string::npos);
    REQUIRE(qmp->count("gdbserver") == 0);
}

TEST_CASE("the debug controller reuses one QMP connection across requests", "[core][debug][controller]")
{
    auto qmp = std::make_shared<FakeQmpState>();

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [qmp]
                                {
                                    return std::make_unique<FakeQmpTransport>(qmp);
                                },
                                []
                                {
                                    return std::make_unique<FakeRspTransport>();
                                }};

    bool        busy        = false;
    bool        unavailable = false;
    std::string error;
    REQUIRE(controller.state(busy, error, unavailable).has_value());
    REQUIRE(controller.state(busy, error, unavailable).has_value());
    REQUIRE(controller.state(busy, error, unavailable).has_value());
    REQUIRE(qmp->opens == 1);
}

TEST_CASE("a watch in progress still answers status and control", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();

    FakeInspector inspector;
    inspector.values = {0xAA};
    std::promise<void> armed;
    auto               armedFuture = armed.get_future();
    inspector.onWatchRead          = [&armed]
    {
        armed.set_value();
    };

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [qmp]
                                {
                                    return std::make_unique<FakeQmpTransport>(qmp);
                                },
                                [transport = std::move(rspTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                &inspector};

    std::optional<mpfshost::DebugWatchEnd> end;
    bool                                   watchBusy        = false;
    bool                                   watchUnavailable = false;
    std::string                            watchError;
    std::thread                            watcher {[&]
                                                    {
                             end = controller.watch({42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 300},
                                                    {},
                                                    {},
                                                    watchBusy,
                                                    watchError,
                                                    watchUnavailable);
                                                    }};

    // The initial watch-value read happens after the guest is paused and the stub is armed.
    armedFuture.wait();

    bool        busy        = false;
    bool        unavailable = false;
    std::string error;
    const auto  status = controller.state(busy, error, unavailable);
    REQUIRE(status.has_value());
    REQUIRE_FALSE(busy);
    REQUIRE(status->runState == mpfshost::DebugRunState::Paused);
    REQUIRE(status->stubActive);

    const auto resumed = controller.control(mpfshost::DebugControlAction::Continue, busy, error, unavailable);
    REQUIRE(resumed.has_value());
    REQUIRE(resumed->runState == mpfshost::DebugRunState::Running);
    REQUIRE(qmp->running);

    watcher.join();
    REQUIRE(end.has_value());
    REQUIRE(qmp->running);
    REQUIRE_FALSE(qmp->stubActive);
}

TEST_CASE("a dropped QMP connection is re-established on the next call", "[core][debug][controller]")
{
    auto qmp = std::make_shared<FakeQmpState>();

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [qmp]
                                {
                                    return std::make_unique<FakeQmpTransport>(qmp);
                                },
                                []
                                {
                                    return std::make_unique<FakeRspTransport>();
                                }};

    bool        busy        = false;
    bool        unavailable = false;
    std::string error;
    REQUIRE(controller.state(busy, error, unavailable).has_value());
    REQUIRE(qmp->opens == 1);

    qmp->closed = true;
    REQUIRE_FALSE(controller.state(busy, error, unavailable).has_value());
    REQUIRE_FALSE(unavailable);
    REQUIRE(error.find("closed") != std::string::npos);

    qmp->closed = false;
    qmp->incoming.emplace_back(R"({"QMP":{"version":{"qemu":{"major":10,"minor":2,"micro":2}}}})");
    REQUIRE(controller.state(busy, error, unavailable).has_value());
    REQUIRE(qmp->opens == 2);
}

TEST_CASE("a QMP command failure does not poison the cached connection", "[core][debug][controller]")
{
    auto qmp = std::make_shared<FakeQmpState>();

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [qmp]
                                {
                                    return std::make_unique<FakeQmpTransport>(qmp);
                                },
                                []
                                {
                                    return std::make_unique<FakeRspTransport>();
                                }};

    bool        busy        = false;
    bool        unavailable = false;
    std::string error;
    REQUIRE(controller.state(busy, error, unavailable).has_value());

    qmp->failCommand = "query-status";
    REQUIRE_FALSE(controller.state(busy, error, unavailable).has_value());
    REQUIRE_FALSE(unavailable);
    REQUIRE(error.find("fake command failed") != std::string::npos);

    qmp->failCommand.clear();
    REQUIRE(controller.state(busy, error, unavailable).has_value());
    REQUIRE(qmp->opens == 1);
}

TEST_CASE("a QMP connect failure is reported as unavailable", "[core][debug][controller]")
{
    auto qmp       = std::make_shared<FakeQmpState>();
    qmp->openFails = true;

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [qmp]
                                {
                                    return std::make_unique<FakeQmpTransport>(qmp);
                                },
                                []
                                {
                                    return std::make_unique<FakeRspTransport>();
                                }};

    bool        busy        = false;
    bool        unavailable = false;
    std::string error;
    REQUIRE_FALSE(controller.state(busy, error, unavailable).has_value());
    REQUIRE(unavailable);
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("the request deadline bounds a pending QMP command", "[core][debug][controller]")
{
    auto qmp = std::make_shared<FakeQmpState>();

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [qmp]
                                {
                                    return std::make_unique<FakeQmpTransport>(qmp);
                                },
                                []
                                {
                                    return std::make_unique<FakeRspTransport>();
                                }};

    bool        busy        = false;
    bool        unavailable = false;
    std::string error;
    REQUIRE(controller.state(busy, error, unavailable).has_value());

    qmp->stall = true;
    mpfshost::RequestDeadline deadline;
    deadline.restart(40ms);
    mpfshost::RequestDeadlineScope scope {deadline};

    const auto start = std::chrono::steady_clock::now();
    REQUIRE_FALSE(controller.state(busy, error, unavailable).has_value());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE_FALSE(unavailable);
    REQUIRE(elapsed < std::chrono::seconds {1});
    REQUIRE(qmp->lastReadTimeout > 0ms);
    REQUIRE(qmp->lastReadTimeout <= 40ms);
}

TEST_CASE("a control pause during a watch leaves the guest paused", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();

    FakeInspector inspector;
    inspector.values = {0xAA};
    std::promise<void> armed;
    auto               armedFuture = armed.get_future();
    inspector.onWatchRead          = [&armed]
    {
        armed.set_value();
    };

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [qmp]
                                {
                                    return std::make_unique<FakeQmpTransport>(qmp);
                                },
                                [transport = std::move(rspTransport)]() mutable
                                {
                                    return std::move(transport);
                                },
                                &inspector};

    std::optional<mpfshost::DebugWatchEnd> end;
    bool                                   watchBusy        = false;
    bool                                   watchUnavailable = false;
    std::string                            watchError;
    std::thread                            watcher {[&]
                                                    {
                             end = controller.watch({42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 300},
                                                    {},
                                                    {},
                                                    watchBusy,
                                                    watchError,
                                                    watchUnavailable);
                                                    }};

    armedFuture.wait();

    bool        busy        = false;
    bool        unavailable = false;
    std::string error;
    REQUIRE(controller.control(mpfshost::DebugControlAction::Stop, busy, error, unavailable).has_value());
    REQUIRE_FALSE(qmp->running);

    watcher.join();
    REQUIRE(end.has_value());
    REQUIRE_FALSE(qmp->running);
    REQUIRE_FALSE(qmp->stubActive);

    const auto paused = controller.state(busy, error, unavailable);
    REQUIRE(paused.has_value());
    REQUIRE(paused->runState == mpfshost::DebugRunState::Paused);
    REQUIRE(paused->hostPaused);

    REQUIRE(controller.control(mpfshost::DebugControlAction::Continue, busy, error, unavailable).has_value());
    REQUIRE(qmp->running);
}
