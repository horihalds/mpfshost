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
        bool                      failZeroTimeout {false};
        std::string               failCommand;
        std::string               architecture {"x86_64"};
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
                if (_state->architecture.empty())
                {
                    _state->incoming.emplace_back(R"({"return":[{},{"cpu-index":1}]})");
                }
                else
                {
                    _state->incoming.emplace_back(R"({"return":[{"cpu-index":0,"target":")" + _state->architecture
                                                  + R"("},{"cpu-index":1,"target":")" + _state->architecture
                                                  + R"("}]})");
                }
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
            if (_state->failZeroTimeout && timeout <= std::chrono::milliseconds::zero())
            {
                closed = false;
                error  = "the fake QMP has no time budget";
                return false;
            }
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

        std::string                                        pending;
        std::string                                        registersXml;
        std::string                                        registerBytes;
        std::string                                        selectedThread;
        std::vector<std::pair<std::string, std::string>>   registerBytesByThread;
        std::vector<std::pair<std::string, std::uint64_t>> cr3ByThread;
        bool                                               noAck {false};
        bool                                               fail {false};
        std::deque<StopReply>                              stops;
        std::vector<std::string>                           threads {"1.1"};
        std::uint64_t                                      currentRip {};
        std::uint64_t                                      currentRsp {};
        std::uint64_t                                      currentCr3 {};
        std::size_t                                        watchInserts {0};
        std::size_t                                        watchRemoves {0};
        std::size_t                                        readWatchInserts {0};
        std::size_t                                        readWatchRemoves {0};
        std::size_t                                        accessWatchInserts {0};
        std::size_t                                        accessWatchRemoves {0};
        bool                                               rejectReadWatch {false};
        bool                                               rejectAccessWatch {false};
        std::size_t                                        breakpointInserts {0};
        std::size_t                                        breakpointRemoves {0};
        std::size_t                                        continues {0};
        std::size_t                                        steps {0};

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
                    selectedThread = thread;
                    quoteAck("OK");
                }
                else
                {
                    quoteAck("E22");
                }
            }
            else if (body == "g")
            {
                const auto found = std::find_if(registerBytesByThread.begin(),
                                                registerBytesByThread.end(),
                                                [this](const auto& entry)
                                                {
                                                    return entry.first == selectedThread;
                                                });
                quoteAck(found != registerBytesByThread.end() ? found->second : registerBytes);
            }
            else if (body.starts_with("Z2,") || body.starts_with("Z3,") || body.starts_with("Z4,"))
            {
                ++watchInserts;
                if (body.starts_with("Z3,"))
                {
                    ++readWatchInserts;
                }
                if (body.starts_with("Z4,"))
                {
                    ++accessWatchInserts;
                }
                if ((body.starts_with("Z3,") && rejectReadWatch) || (body.starts_with("Z4,") && rejectAccessWatch))
                {
                    quoteAck("");
                }
                else
                {
                    quoteAck("OK");
                }
            }
            else if (body.starts_with("z2,") || body.starts_with("z3,") || body.starts_with("z4,"))
            {
                ++watchRemoves;
                if (body.starts_with("z3,"))
                {
                    ++readWatchRemoves;
                }
                if (body.starts_with("z4,"))
                {
                    ++accessWatchRemoves;
                }
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
                    value             = currentCr3;
                    const auto thread = std::find_if(cr3ByThread.begin(),
                                                     cr3ByThread.end(),
                                                     [this](const auto& entry)
                                                     {
                                                         return entry.first == selectedThread;
                                                     });
                    if (thread != cr3ByThread.end())
                    {
                        value = thread->second;
                    }
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

    std::string aarch64RegisterXml()
    {
        return "<feature><reg name=\"x0\" bitsize=\"64\"/><reg name=\"x1\" bitsize=\"64\"/><reg name=\"sp\" "
               "bitsize=\"64\"/><reg name=\"pc\" bitsize=\"64\"/></feature>";
    }

    std::string registerBytes(std::uint64_t rip = 0xffffffffae89ab1bull,
                              std::uint64_t rsp = 0,
                              std::uint64_t cr3 = 0x1a2b3000ull,
                              std::uint64_t rbp = 0)
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
        put(48, rbp);                  // rbp
        put(56, rsp);                  // rsp
        put(128, rip);                 // rip
        put(140, 0x2468ace0ull);       // cr0
        put(148, cr3);                 // cr3
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
        bool                                                                                     hasModule {true};
        std::size_t                                                                              memoryReads {0};
        std::function<void()>                                                                    onWatchRead;
        std::function<bool(std::uint64_t, std::uint32_t, std::vector<std::byte>&, std::string&)> memoryHandler;

        struct GuestMapping
        {
            std::uint64_t cr3 {};
            std::uint32_t pid {};
            std::uint32_t tid {};
            bool          threadFound {true};
        };

        std::vector<GuestMapping> guests;

        [[nodiscard]] std::optional<mpfshost::debug::DebugGuestThread>
        guestThread(std::uint64_t cr3, std::uint64_t, std::uint64_t, std::string&) override
        {
            for (const auto& guest : guests)
            {
                if (guest.cr3 == cr3)
                {
                    return mpfshost::debug::DebugGuestThread {guest.pid, guest.tid, guest.threadFound};
                }
            }
            return std::nullopt;
        }

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

        std::function<mpfshost::debug::DebugSymbolHit(const std::string&, const std::string&)> resolveHandler;

        [[nodiscard]] mpfshost::debug::DebugSymbolHit
        resolveSymbol(std::uint32_t, const std::string& module, const std::string& symbol, std::string&) override
        {
            if (resolveHandler)
            {
                return resolveHandler(module, symbol);
            }
            return {};
        }

        [[nodiscard]] bool readMemory(std::uint32_t,
                                      std::uint64_t           address,
                                      std::uint32_t           size,
                                      std::vector<std::byte>& out,
                                      std::string&            error) override
        {
            ++memoryReads;
            out.clear();
            if (memoryHandler)
            {
                return memoryHandler(address, size, out, error);
            }
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

TEST_CASE("the debug controller lists stub threads and resolves guest contexts", "[core][debug][controller]")
{
    auto  qmp                  = std::make_shared<FakeQmpState>();
    auto  qmpTransport         = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport         = std::make_unique<FakeRspTransport>();
    auto* rsp                  = rspTransport.get();
    rsp->registersXml          = x86RegisterXml();
    rsp->threads               = {"p01.01", "p01.02", "p01.03"};
    rsp->registerBytesByThread = {
        {"p01.01", registerBytes(0x1111, 0x5000, 0x1000)},
        {"p01.02", registerBytes(0x2222, 0x6000, 0x2000)},
        {"p01.03", registerBytes(0x3333, 0x7000, 0x3000)},
    };

    FakeInspector inspector;
    inspector.guests = {
        {0x1000, 42, 100,  true},
        {0x2000, 43,   0, false},
    };

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
    const auto  threads     = controller.threads({}, invalidState, busy, error, unavailable);
    REQUIRE(threads.has_value());
    REQUIRE_FALSE(invalidState);
    REQUIRE(threads->size() == 3);

    REQUIRE((*threads)[0].vcpu == 0);
    REQUIRE((*threads)[0].threadId == "p01.01");
    REQUIRE((*threads)[0].rip == 0x1111);
    REQUIRE((*threads)[0].rsp == 0x5000);
    REQUIRE((*threads)[0].cr3 == 0x1000);
    REQUIRE((*threads)[0].processFound);
    REQUIRE((*threads)[0].pid == 42);
    REQUIRE((*threads)[0].threadFound);
    REQUIRE((*threads)[0].tid == 100);

    REQUIRE((*threads)[1].processFound);
    REQUIRE((*threads)[1].pid == 43);
    REQUIRE_FALSE((*threads)[1].threadFound);

    REQUIRE_FALSE((*threads)[2].processFound);
    REQUIRE(inspector.memoryReads == 0); // thread identity comes from the thread map, not guest memory

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
    REQUIRE(status->architecture == "x86_64");
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
    const auto  response    = controller.detach(1, busy, error, unavailable);
    REQUIRE(response.has_value());
    REQUIRE(response->stubStopped);
    REQUIRE(response->foreignStub);
    REQUIRE_FALSE(response->guestResumed);
    REQUIRE_FALSE(response->detached);
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

    const auto response = controller.detach(1, busy, error, unavailable);
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
        {42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 5000, {}, {}},
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

TEST_CASE("a read watchpoint uses Z3 and reports its access mode", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->stops.push_back({"1.1", 0x7ff84522b1f1ull, 0x1234, 0x200123, 0x5000});

    FakeInspector inspector;
    inspector.values = {0xAA, 0xAA};

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
        {42, 0x5000, 4, mpfshost::DebugWatchAccess::Read, 1, 5000, {}, {}},
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
    REQUIRE(end->access == mpfshost::DebugWatchAccess::Read);
    REQUIRE(end->hitCount == 1);
    REQUIRE(hits.size() == 1);
    REQUIRE(rsp->readWatchInserts == 1);
    REQUIRE(rsp->readWatchRemoves == 1);
}

TEST_CASE("an unsupported read watchpoint is reported as unavailable", "[core][debug][controller]")
{
    auto qmp             = std::make_shared<FakeQmpState>();
    qmp->running         = true;
    auto  qmpTransport   = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport   = std::make_unique<FakeRspTransport>();
    auto* rsp            = rspTransport.get();
    rsp->registersXml    = x86RegisterXml();
    rsp->rejectReadWatch = true;

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

    bool        busy = false;
    std::string error;
    bool        unavailable = false;
    bool        notFound    = false;
    const auto  end         = controller.watch({42, 0x5000, 4, mpfshost::DebugWatchAccess::Read, 1, 5000, {}, {}},
                                               {},
                                               {},
                                               {},
                                               busy,
                                               error,
                                               unavailable,
                                               notFound);
    REQUIRE_FALSE(end.has_value());
    REQUIRE(unavailable);
    REQUIRE(error.find("read watchpoints") != std::string::npos);
}

TEST_CASE("an unlimited watch runs until cancellation", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->stops.push_back({"1.1", 0x7ff84522b1f1ull, 0x1234, 0x200123, 0x5000});

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
    std::size_t                           aborts = 0;
    bool                                  busy   = false;
    std::string                           error;
    bool                                  unavailable = false;
    const auto                            end         = controller.watch(
        {42, 0x5000, 4, mpfshost::DebugWatchAccess::Write, 0, 5000, {}, {}},
        [&hits](const mpfshost::DebugWatchData& hit)
        {
            hits.push_back(hit);
            return true;
        },
        [&aborts]()
        {
            return ++aborts > 1;
        },
        busy,
        error,
        unavailable);
    REQUIRE(end.has_value());
    REQUIRE(end->status == mpfshost::DebugWatchStatus::Cancelled);
    REQUIRE(end->hitCount == 1);
    REQUIRE(hits.size() == 1);
    REQUIRE(rsp->watchRemoves == 1);
}

TEST_CASE("a quiet watch emits heartbeats before it times out", "[core][debug][controller]")
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

    std::vector<mpfshost::DebugWatchHeartbeat> heartbeats;
    bool                                       busy = false;
    std::string                                error;
    bool                                       unavailable = false;
    bool                                       notFound    = false;
    const auto                                 end         = controller.watch(
        {42, 0x5000, 4, mpfshost::DebugWatchAccess::Access, 1, 1100, {}, {}},
        {},
        [&heartbeats](const mpfshost::DebugWatchHeartbeat& heartbeat)
        {
            heartbeats.push_back(heartbeat);
            return true;
        },
        {},
        busy,
        error,
        unavailable,
        notFound);
    REQUIRE(end.has_value());
    REQUIRE(end->status == mpfshost::DebugWatchStatus::Timeout);
    REQUIRE_FALSE(heartbeats.empty());
    REQUIRE(heartbeats.front().access == mpfshost::DebugWatchAccess::Access);
    REQUIRE(heartbeats.front().elapsedMs >= 1000);
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
        {42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 5000, {}, {}},
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
        {42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 5000, {}, {}},
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
        {42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 5000, {}, {}},
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
        {42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 100, {}, {}},
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
        {42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 100, {}, {}},
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
    const auto  end         = controller.watch(
        {42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 5000, {}, {}}, {}, {}, busy, error, unavailable);
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
    const auto  end         = controller.watch(
        {42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 5, {}, {}}, {}, {}, busy, error, unavailable);
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
        {42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 5000, {}, {}},
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
        {42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 2, 5000, {}, {}},
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
    const auto  end         = controller.watch(
        {42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 5000, {}, {}}, {}, {}, busy, error, unavailable);
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
        rsp->cr3ByThread   = {
            {"1.1", 0x200123}
        };
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
            controller.breakpoint({42, 0x7ff84522b1f1ull, mpfshost::DebugBreakpointKind::Software, 5000, {}, {}},
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
        REQUIRE_FALSE(response->unverified);
        REQUIRE(rsp->selectedThread == "1.1");
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
            controller.breakpoint({42, 0x7ff84522b1f1ull, mpfshost::DebugBreakpointKind::Hardware, 5000, {}, {}},
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

TEST_CASE("a breakpoint resolves an export or PDB symbol before arming", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->stops.push_back({"1.1", 0x7ff84522b190ull, 0x1234, 0x200123});
    rsp->stops.push_back({"1.1", 0xfffff80000123456ull, 0x1234, 0x1ae000});

    FakeInspector inspector;
    inspector.resolveHandler = [](const std::string& module, const std::string& symbol)
    {
        if (module == "msvcrt.dll" && symbol == "memcpy")
        {
            return mpfshost::debug::DebugSymbolHit {true, false, 0x7ff84522b180ull};
        }
        if (module == "nt" && symbol == "HalProcessorIdle")
        {
            return mpfshost::debug::DebugSymbolHit {true, false, 0xfffff80000123456ull};
        }
        return mpfshost::debug::DebugSymbolHit {};
    };

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
    bool        notFound    = false;

    const auto exported =
        controller.breakpoint({42, 0x10, mpfshost::DebugBreakpointKind::Hardware, 5000, "msvcrt.dll", "memcpy"},
                              {},
                              invalidState,
                              busy,
                              error,
                              unavailable,
                              notFound);
    REQUIRE(exported.has_value());
    REQUIRE_FALSE(notFound);
    REQUIRE(exported->address == 0x7ff84522b190ull);
    REQUIRE(rsp->breakpointInserts == 1);
}

TEST_CASE("a breakpoint resolves a PDB symbol before arming", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->stops.push_back({"1.1", 0xfffff80000123456ull, 0x1234, 0x1ae000});

    FakeInspector inspector;
    inspector.resolveHandler = [](const std::string& module, const std::string& symbol)
    {
        if (module == "nt" && symbol == "HalProcessorIdle")
        {
            return mpfshost::debug::DebugSymbolHit {true, false, 0xfffff80000123456ull};
        }
        return mpfshost::debug::DebugSymbolHit {};
    };

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
    bool        notFound    = false;

    const auto pdb =
        controller.breakpoint({42, 0, mpfshost::DebugBreakpointKind::Hardware, 5000, "nt", "HalProcessorIdle"},
                              {},
                              invalidState,
                              busy,
                              error,
                              unavailable,
                              notFound);
    REQUIRE(pdb.has_value());
    REQUIRE_FALSE(notFound);
    REQUIRE(pdb->address == 0xfffff80000123456ull);
    REQUIRE(rsp->breakpointInserts == 1);
}

TEST_CASE("a symbolic breakpoint reports a missing module or symbol as not found", "[core][debug][controller]")
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
    bool        notFound    = false;

    const auto missing =
        controller.breakpoint({42, 0, mpfshost::DebugBreakpointKind::Software, 5000, "nt", "NoSuchSymbol"},
                              {},
                              invalidState,
                              busy,
                              error,
                              unavailable,
                              notFound);
    REQUIRE_FALSE(missing.has_value());
    REQUIRE(notFound);
    REQUIRE_FALSE(busy);
    REQUIRE_FALSE(invalidState);
    REQUIRE_FALSE(error.empty());
    REQUIRE(rsp->breakpointInserts == 0);

    const auto missingModule =
        controller.breakpoint({42, 0, mpfshost::DebugBreakpointKind::Software, 5000, "ghost", {}},
                              {},
                              invalidState,
                              busy,
                              error,
                              unavailable,
                              notFound);
    REQUIRE_FALSE(missingModule.has_value());
    REQUIRE(notFound);
    REQUIRE(error.find("ghost") != std::string::npos);
    REQUIRE(rsp->breakpointInserts == 0);
}

TEST_CASE("an ordinal-only export stays unlabelled in a breakpoint hit", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->stops.push_back({"1.1", 0x7ff84522b1f1ull, 0x1234, 0x200123});

    FakeInspector inspector;
    inspector.exportHit = mpfshost::debug::DebugExportHit {true, 0x7ff84522b180ull, 0x71, {}};
    inspector.pdb       = std::nullopt;

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
    bool        notFound    = false;
    const auto  response =
        controller.breakpoint({42, 0x7ff84522b1f1ull, mpfshost::DebugBreakpointKind::Hardware, 5000, {}, {}},
                              {},
                              invalidState,
                              busy,
                              error,
                              unavailable,
                              notFound);
    REQUIRE(response.has_value());
    REQUIRE(response->status == mpfshost::DebugBreakpointStatus::Hit);
    REQUIRE(response->exportFound);
    REQUIRE(response->exportName.empty());
    REQUIRE(response->exportOffset == 0x71);
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
    const auto  response =
        controller.breakpoint({42, 0x7ff84522b1f1ull, mpfshost::DebugBreakpointKind::Hardware, 5000, {}, {}},
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
    const auto  response =
        controller.breakpoint({42, 0x7ff8451b1234ull, mpfshost::DebugBreakpointKind::Hardware, 5000, {}, {}},
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

TEST_CASE("a shadow-CR3 hardware hit in a target module is accepted and flagged", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->stops.push_back({"1.1", 0x7ff84522b1f1ull, 0x1234, 0x999000});

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
        controller.breakpoint({42, 0x7ff84522b1f1ull, mpfshost::DebugBreakpointKind::Hardware, 5000, {}, {}},
                              {},
                              invalidState,
                              busy,
                              error,
                              unavailable);
    REQUIRE(response.has_value());
    REQUIRE(response->status == mpfshost::DebugBreakpointStatus::Hit);
    REQUIRE(response->skippedCount == 0);
    REQUIRE(response->cr3 == 0x999000);
    REQUIRE(response->unverified);
}

TEST_CASE("a matching-PC hardware stop in another process is skipped", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->stops.push_back({"1.1", 0x12345678, 0x1234, 0x999000});
    rsp->stops.push_back({"1.1", 0x12345678, 0x1234, 0x200123});

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
    const auto response = controller.breakpoint({42, 0x12345678, mpfshost::DebugBreakpointKind::Hardware, 5000, {}, {}},
                                                {},
                                                invalidState,
                                                busy,
                                                error,
                                                unavailable);
    REQUIRE(response.has_value());
    REQUIRE(response->status == mpfshost::DebugBreakpointStatus::Hit);
    REQUIRE(response->skippedCount == 1);
    REQUIRE(response->cr3 == 0x200123);
    REQUIRE_FALSE(response->unverified);
    REQUIRE(rsp->continues == 2);
}

TEST_CASE("a software user-mode breakpoint selects a vCPU in the target process", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->threads       = {"1.1", "1.2"};
    rsp->cr3ByThread   = {
        {"1.1", 0x999000},
        {"1.2", 0x200123}
    };
    rsp->stops.push_back({"1.2", 0x7ff84522b1f1ull, 0x1234, 0x200123});

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
        controller.breakpoint({42, 0x7ff84522b1f1ull, mpfshost::DebugBreakpointKind::Software, 5000, {}, {}},
                              {},
                              invalidState,
                              busy,
                              error,
                              unavailable);
    REQUIRE(response.has_value());
    REQUIRE(response->status == mpfshost::DebugBreakpointStatus::Hit);
    REQUIRE(rsp->selectedThread == "1.2");
    REQUIRE(rsp->breakpointInserts == 1);
}

TEST_CASE("a software user-mode breakpoint without a target vCPU reports invalid state", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->threads       = {"1.1", "1.2"};
    rsp->cr3ByThread   = {
        {"1.1", 0x999000},
        {"1.2", 0x999000}
    };

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
        controller.breakpoint({42, 0x7ff84522b1f1ull, mpfshost::DebugBreakpointKind::Software, 5000, {}, {}},
                              {},
                              invalidState,
                              busy,
                              error,
                              unavailable);
    REQUIRE_FALSE(response.has_value());
    REQUIRE(invalidState);
    REQUIRE_FALSE(busy);
    REQUIRE(error.find("user-mode address") != std::string::npos);
    REQUIRE(rsp->breakpointInserts == 0);
    REQUIRE_FALSE(qmp->stubActive);
    REQUIRE(qmp->running);
}

TEST_CASE("a breakpoint times out and still restores the guest", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = true;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->cr3ByThread   = {
        {"1.1", 0x200123}
    };

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
        controller.breakpoint({42, 0x7ff84522b1f1ull, mpfshost::DebugBreakpointKind::Software, 5, {}, {}},
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

TEST_CASE("an expired request budget still tears down a timed-out breakpoint", "[core][debug][controller]")
{
    auto qmp             = std::make_shared<FakeQmpState>();
    qmp->running         = true;
    qmp->failZeroTimeout = true;
    auto  qmpTransport   = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport   = std::make_unique<FakeRspTransport>();
    auto* rsp            = rspTransport.get();
    rsp->registersXml    = x86RegisterXml();

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

    mpfshost::RequestDeadline deadline;
    deadline.restart(100ms);
    mpfshost::RequestDeadlineScope scope {deadline};

    bool        invalidState = false;
    bool        busy         = false;
    std::string error;
    bool        unavailable = false;
    const auto  response =
        controller.breakpoint({42, 0xfffff80000123456ull, mpfshost::DebugBreakpointKind::Hardware, 5000, {}, {}},
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

TEST_CASE("a single step selects an explicit GDB thread", "[core][debug][controller]")
{
    auto  qmp          = std::make_shared<FakeQmpState>();
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->registerBytes = registerBytes(0x7ff84522b1f1ull);
    rsp->threads       = {"p01.01", "p01.02"};
    rsp->stops.push_back({"p01.02", 0x7ff84522b1f1ull, 0x1234, 0x200123});

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
    const auto  response    = controller.step({0, "p01.02"}, invalidState, busy, error, unavailable);
    REQUIRE(response.has_value());
    REQUIRE_FALSE(invalidState);
    REQUIRE(response->record.vcpu == 1);
    REQUIRE(response->record.threadId == "p01.02");
    REQUIRE(rsp->selectedThread == "p01.02");
    REQUIRE(rsp->steps == 1);
}

TEST_CASE("a single step refuses an unknown GDB thread", "[core][debug][controller]")
{
    auto  qmp          = std::make_shared<FakeQmpState>();
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->registerBytes = registerBytes();
    rsp->threads       = {"p01.01"};

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
    const auto  response    = controller.step({0, "p09.99"}, invalidState, busy, error, unavailable);
    REQUIRE_FALSE(response.has_value());
    REQUIRE(invalidState);
    REQUIRE(error.find("p09.99") != std::string::npos);
    REQUIRE(rsp->steps == 0);
    REQUIRE(qmp->count("gdbserver none") == 1);
    REQUIRE_FALSE(qmp->stubActive);
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
                             end = controller.watch({42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 300, {}, {}},
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
                             end = controller.watch({42, 0x5000, 2, mpfshost::DebugWatchAccess::Write, 1, 300, {}, {}},
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

TEST_CASE("the debug controller rejects an architecture QMP reports as aarch64", "[core][debug][controller]")
{
    auto qmp          = std::make_shared<FakeQmpState>();
    qmp->architecture = "aarch64";
    auto rspTransport = std::make_unique<FakeRspTransport>();

    DebugController controller {"/tmp/qmp.sock",
                                "/tmp/stub.sock",
                                [qmp]
                                {
                                    return std::make_unique<FakeQmpTransport>(qmp);
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
    REQUIRE(unavailable);
    REQUIRE_FALSE(invalidState);
    REQUIRE(error.find("aarch64") != std::string::npos);
    REQUIRE(error.find("x86_64") != std::string::npos);
    REQUIRE(qmp->count("gdbserver") == 0);
}

TEST_CASE("the debug controller rejects an aarch64 target description before arming anything",
          "[core][debug][controller]")
{
    auto qmp                   = std::make_shared<FakeQmpState>();
    qmp->architecture          = "";
    auto qmpTransport          = std::make_unique<FakeQmpTransport>(qmp);
    auto rspTransport          = std::make_unique<FakeRspTransport>();
    rspTransport->registersXml = aarch64RegisterXml();

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
    REQUIRE(unavailable);
    REQUIRE_FALSE(invalidState);
    REQUIRE(error.find("aarch64") != std::string::npos);
    REQUIRE(qmp->count("gdbserver unix:") == 1);
    REQUIRE(qmp->count("gdbserver none") == 1);
    REQUIRE_FALSE(qmp->stubActive);
}

TEST_CASE("the debug controller rejects an unidentified target description", "[core][debug][controller]")
{
    auto qmp                   = std::make_shared<FakeQmpState>();
    qmp->architecture          = "";
    auto qmpTransport          = std::make_unique<FakeQmpTransport>(qmp);
    auto rspTransport          = std::make_unique<FakeRspTransport>();
    rspTransport->registersXml = "<feature><reg name=\"foo\" bitsize=\"64\"/></feature>";

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
    REQUIRE(unavailable);
    REQUIRE(error.find("does not identify") != std::string::npos);
    REQUIRE(qmp->count("gdbserver unix:") == 1);
    REQUIRE(qmp->count("gdbserver none") == 1);
}

TEST_CASE("debug status reports the architecture of an unsupported guest", "[core][debug][controller]")
{
    auto qmp          = std::make_shared<FakeQmpState>();
    qmp->architecture = "aarch64";

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
    REQUIRE_FALSE(unavailable);
    REQUIRE(status->architecture == "aarch64");
    REQUIRE(status->runState == mpfshost::DebugRunState::Paused);
}

TEST_CASE("debug attach pauses the guest and detach restores it", "[core][debug][controller]")
{
    auto qmp                   = std::make_shared<FakeQmpState>();
    qmp->running               = true;
    auto qmpTransport          = std::make_unique<FakeQmpTransport>(qmp);
    auto rspTransport          = std::make_unique<FakeRspTransport>();
    rspTransport->registersXml = x86RegisterXml();

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
    const auto  attached    = controller.attach(7, invalidState, busy, error, unavailable);
    REQUIRE(attached.has_value());
    REQUIRE_FALSE(invalidState);
    REQUIRE_FALSE(busy);
    REQUIRE(attached->guestPaused);
    REQUIRE(attached->stubMode == "unix");
    REQUIRE(attached->status.stubOwned);
    REQUIRE(attached->status.stubActive);
    REQUIRE_FALSE(attached->status.foreignStub);
    REQUIRE(attached->status.architecture == "x86_64");
    REQUIRE_FALSE(qmp->running);
    REQUIRE(qmp->stubActive);

    const auto second = controller.attach(8, invalidState, busy, error, unavailable);
    REQUIRE_FALSE(second.has_value());
    REQUIRE_FALSE(invalidState);
    REQUIRE(busy);
    REQUIRE(error.find("already attached") != std::string::npos);

    busy                = false;
    const auto detached = controller.detach(7, busy, error, unavailable);
    REQUIRE(detached.has_value());
    REQUIRE(detached->detached);
    REQUIRE(detached->stubStopped);
    REQUIRE_FALSE(detached->foreignStub);
    REQUIRE(detached->guestResumed);
    REQUIRE_FALSE(detached->status.stubActive);
    REQUIRE(qmp->running);
    REQUIRE_FALSE(qmp->stubActive);
}

TEST_CASE("detach leaves a guest that was already paused alone", "[core][debug][controller]")
{
    auto qmp                   = std::make_shared<FakeQmpState>();
    qmp->running               = false;
    auto qmpTransport          = std::make_unique<FakeQmpTransport>(qmp);
    auto rspTransport          = std::make_unique<FakeRspTransport>();
    rspTransport->registersXml = x86RegisterXml();

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
    const auto  attached    = controller.attach(7, invalidState, busy, error, unavailable);
    REQUIRE(attached.has_value());
    REQUIRE_FALSE(attached->guestPaused);
    REQUIRE_FALSE(qmp->running);

    const auto detached = controller.detach(7, busy, error, unavailable);
    REQUIRE(detached.has_value());
    REQUIRE_FALSE(detached->guestResumed);
    REQUIRE_FALSE(qmp->running);
}

TEST_CASE("attach refuses a foreign stub and names debug-detach", "[core][debug][controller]")
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

    bool        invalidState = false;
    bool        busy         = false;
    std::string error;
    bool        unavailable = false;
    const auto  attached    = controller.attach(7, invalidState, busy, error, unavailable);
    REQUIRE_FALSE(attached.has_value());
    REQUIRE(invalidState);
    REQUIRE_FALSE(busy);
    REQUIRE(error.find("debug-detach") != std::string::npos);
    REQUIRE(qmp->stubActive);
    REQUIRE(qmp->count("gdbserver unix:") == 0);
}

TEST_CASE("detachSession only ends the owning session's attachment", "[core][debug][controller]")
{
    auto qmp                   = std::make_shared<FakeQmpState>();
    qmp->running               = true;
    auto qmpTransport          = std::make_unique<FakeQmpTransport>(qmp);
    auto rspTransport          = std::make_unique<FakeRspTransport>();
    rspTransport->registersXml = x86RegisterXml();

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
    REQUIRE(controller.attach(7, invalidState, busy, error, unavailable).has_value());

    controller.detachSession(8);
    REQUIRE(qmp->stubActive);
    REQUIRE_FALSE(qmp->running);

    controller.detachSession(7);
    REQUIRE_FALSE(qmp->stubActive);
    REQUIRE(qmp->running);
}

TEST_CASE("an attached stub is reused by a register read", "[core][debug][controller]")
{
    auto qmp                    = std::make_shared<FakeQmpState>();
    qmp->running                = false;
    auto qmpTransport           = std::make_unique<FakeQmpTransport>(qmp);
    auto rspTransport           = std::make_unique<FakeRspTransport>();
    rspTransport->registersXml  = x86RegisterXml();
    rspTransport->registerBytes = registerBytes();

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
    REQUIRE(controller.attach(7, invalidState, busy, error, unavailable).has_value());

    const auto records = controller.registers({}, invalidState, busy, error, unavailable);
    REQUIRE(records.has_value());
    REQUIRE(records->size() == 1);
    REQUIRE(records->front().rip == 0xffffffffae89ab1bull);
    REQUIRE(qmp->stubActive);
    REQUIRE(qmp->count("gdbserver unix:") == 1);

    const auto detached = controller.detach(7, busy, error, unavailable);
    REQUIRE(detached.has_value());
    REQUIRE(detached->detached);
    REQUIRE_FALSE(qmp->stubActive);
}

TEST_CASE("an attached arm drains the pending stop left by a QMP pause", "[core][debug][controller]")
{
    auto qmp                    = std::make_shared<FakeQmpState>();
    qmp->running                = false;
    auto qmpTransport           = std::make_unique<FakeQmpTransport>(qmp);
    auto rspTransport           = std::make_unique<FakeRspTransport>();
    rspTransport->registersXml  = x86RegisterXml();
    rspTransport->registerBytes = registerBytes();
    auto* rsp                   = rspTransport.get();

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
    REQUIRE(controller.attach(7, invalidState, busy, error, unavailable).has_value());
    REQUIRE(controller.continueGuest(7, invalidState, busy, error, unavailable).has_value());

    // The stub is running; a QMP stop produces a stop reply that is still sitting on the RSP stream.
    rsp->pending += rspPacket(FakeRspTransport::stopPacket({"1.1", 0x1234, 0x2000, 0x3000}));
    REQUIRE(controller.control(mpfshost::DebugControlAction::Stop, busy, error, unavailable).has_value());

    const auto armed = controller.armWatch(
        7, {42, 0x5000, 4, mpfshost::DebugWatchAccess::Write}, invalidState, busy, error, unavailable);
    INFO(error);
    REQUIRE(armed.has_value());
    REQUIRE(armed->slot == 0);

    REQUIRE(controller.detach(7, busy, error, unavailable).has_value());
}

TEST_CASE("persistent breakpoints share four hardware slots and keep their ids", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = false;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();

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
    REQUIRE(controller.attach(7, invalidState, busy, error, unavailable).has_value());

    std::vector<std::uint32_t> ids;
    for (std::uint32_t index = 0; index < mpfshost::kMaxDebugHardwareSlots; ++index)
    {
        const auto armed = controller.armBreakpoint(
            7, {42, 0x1000 + index, mpfshost::DebugBreakpointKind::Hardware}, invalidState, busy, error, unavailable);
        REQUIRE(armed.has_value());
        REQUIRE(armed->slot == index);
        ids.push_back(armed->id);
    }

    const auto exhausted = controller.armBreakpoint(
        7, {42, 0x2000, mpfshost::DebugBreakpointKind::Hardware}, invalidState, busy, error, unavailable);
    REQUIRE_FALSE(exhausted.has_value());
    REQUIRE(invalidState);
    REQUIRE(error.find("hardware debug slots") != std::string::npos);

    const auto software = controller.armBreakpoint(7,
                                                   {42, 0xfffff80000123456ull, mpfshost::DebugBreakpointKind::Software},
                                                   invalidState,
                                                   busy,
                                                   error,
                                                   unavailable);
    REQUIRE(software.has_value());
    REQUIRE(software->slot == mpfshost::kNoDebugHardwareSlot);

    const auto list = controller.listBreakpoints(7, invalidState, busy, error, unavailable);
    REQUIRE(list.has_value());
    REQUIRE(list->breakpoints.size() == mpfshost::kMaxDebugHardwareSlots + 1);

    // Removing the breakpoint that holds slot 1 frees it without renumbering the others.
    const auto removed = controller.removeBreakpoint(7, {ids[1]}, invalidState, busy, error, unavailable);
    REQUIRE(removed.has_value());
    REQUIRE(removed->removed);

    const auto replacement = controller.armBreakpoint(
        7, {42, 0x4000, mpfshost::DebugBreakpointKind::Hardware}, invalidState, busy, error, unavailable);
    REQUIRE(replacement.has_value());
    REQUIRE(replacement->slot == 1);
    REQUIRE(replacement->id > ids.back());

    // Free the slot the third original breakpoint holds, then arm a watchpoint with that slot.
    REQUIRE(controller.removeBreakpoint(7, {ids[2]}, invalidState, busy, error, unavailable)->removed);
    const auto watched = controller.armWatch(
        7, {42, 0x5000, 4, mpfshost::DebugWatchAccess::Access}, invalidState, busy, error, unavailable);
    REQUIRE(watched.has_value());
    REQUIRE(watched->slot == 2);
    REQUIRE(watched->access == mpfshost::DebugWatchAccess::Access);

    const auto detached = controller.detach(7, busy, error, unavailable);
    REQUIRE(detached.has_value());
    REQUIRE(detached->detached);
    REQUIRE_FALSE(qmp->stubActive);
    REQUIRE(rsp->breakpointRemoves == 6);
    REQUIRE(rsp->watchRemoves == 1);
    REQUIRE(rsp->accessWatchInserts == 1);
    REQUIRE(rsp->accessWatchRemoves == 1);

    REQUIRE_FALSE(controller.listBreakpoints(7, invalidState, busy, error, unavailable).has_value());
    REQUIRE(invalidState);
}

TEST_CASE("a wait reports breakpoint and watchpoint hits over repeated requests", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = false;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();
    rsp->threads       = {"1.1", "1.2"};
    rsp->registerBytes = registerBytes(0x7ff84522b1f1ull);
    rsp->stops.push_back({"1.1", 0x7ff84522b1f1ull, 0x1234, 0x1a2b3000});
    rsp->stops.push_back({"1.2", 0x7ff84522b200ull, 0x5678, 0x1a2b3000, 0x5000});

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
    REQUIRE(controller.attach(7, invalidState, busy, error, unavailable).has_value());

    const auto breakpoint = controller.armBreakpoint(
        7, {42, 0x7ff84522b1f1ull, mpfshost::DebugBreakpointKind::Hardware}, invalidState, busy, error, unavailable);
    REQUIRE(breakpoint.has_value());
    const auto watch = controller.armWatch(
        7, {42, 0x5000, 2, mpfshost::DebugWatchAccess::Write}, invalidState, busy, error, unavailable);
    REQUIRE(watch.has_value());
    REQUIRE(breakpoint->id != watch->id);

    std::vector<mpfshost::DebugWaitHit> hits;
    const auto                          first = controller.wait(
        {5000, 1},
        [&hits](const mpfshost::DebugWaitHit& hit)
        {
            hits.push_back(hit);
            return true;
        },
        {},
        7,
        invalidState,
        busy,
        error,
        unavailable);
    REQUIRE(first.has_value());
    REQUIRE(first->status == mpfshost::DebugWatchStatus::Completed);
    REQUIRE(first->hitCount == 1);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits.front().id == breakpoint->id);
    REQUIRE(hits.front().kind == mpfshost::DebugResourceKind::Breakpoint);
    REQUIRE(hits.front().record.rip == 0x7ff84522b1f1ull);
    REQUIRE(hits.front().module == "msvcrt.dll");
    // The stop CR3 is not the process DTB, so the module match is accepted but marked unverified.
    REQUIRE(hits.front().unverified);

    // The second wait resumes the stopped guest and reports the watchpoint hit with its own id.
    rsp->registerBytes = registerBytes(0x7ff84522b200ull);
    const auto second  = controller.wait(
        {5000, 1},
        [&hits](const mpfshost::DebugWaitHit& hit)
        {
            hits.push_back(hit);
            return true;
        },
        {},
        7,
        invalidState,
        busy,
        error,
        unavailable);
    REQUIRE(second.has_value());
    REQUIRE(second->hitCount == 1);
    REQUIRE(hits.size() == 2);
    REQUIRE(hits.back().id == watch->id);
    REQUIRE(hits.back().kind == mpfshost::DebugResourceKind::Watchpoint);
    REQUIRE(hits.back().address == 0x5000);

    const auto removed = controller.removeWatch(7, {watch->id}, invalidState, busy, error, unavailable);
    REQUIRE(removed.has_value());
    REQUIRE(removed->removed);
    REQUIRE(rsp->watchRemoves == 1);

    const auto watches = controller.listWatches(7, invalidState, busy, error, unavailable);
    REQUIRE(watches.has_value());
    REQUIRE(watches->watches.empty());

    const auto detached = controller.detach(7, busy, error, unavailable);
    REQUIRE(detached.has_value());
    REQUIRE(detached->detached);
    REQUIRE_FALSE(qmp->stubActive);
}

TEST_CASE("a wait skips a hardware breakpoint stop from another process", "[core][debug][controller]")
{
    auto qmp                   = std::make_shared<FakeQmpState>();
    qmp->running               = false;
    auto  qmpTransport         = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport         = std::make_unique<FakeRspTransport>();
    auto* rsp                  = rspTransport.get();
    rsp->registersXml          = x86RegisterXml();
    rsp->threads               = {"1.1", "1.2"};
    rsp->registerBytesByThread = {
        {"1.1", registerBytes(0x12345678, 0x1234, 0x1ae000)},
        {"1.2", registerBytes(0x12345678, 0x1234, 0x999000)},
    };
    rsp->stops.push_back({"1.2", 0x12345678, 0x1234, 0x999000});
    rsp->stops.push_back({"1.1", 0x12345678, 0x1234, 0x1ae000});

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
    REQUIRE(controller.attach(7, invalidState, busy, error, unavailable).has_value());
    REQUIRE(
        controller
            .armBreakpoint(
                7, {42, 0x12345678, mpfshost::DebugBreakpointKind::Hardware}, invalidState, busy, error, unavailable)
            .has_value());

    std::vector<mpfshost::DebugWaitHit> hits;
    const auto                          end = controller.wait(
        {5000, 1},
        [&hits](const mpfshost::DebugWaitHit& hit)
        {
            hits.push_back(hit);
            return true;
        },
        {},
        7,
        invalidState,
        busy,
        error,
        unavailable);
    REQUIRE(end.has_value());
    REQUIRE(end->hitCount == 1);
    REQUIRE(end->skippedCount == 1);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits.front().record.cr3 == 0x1ae000);
    REQUIRE_FALSE(hits.front().unverified);
    REQUIRE(rsp->continues == 2);
}

TEST_CASE("a cancelled wait keeps the armed resources and detach clears them", "[core][debug][controller]")
{
    auto qmp           = std::make_shared<FakeQmpState>();
    qmp->running       = false;
    auto  qmpTransport = std::make_unique<FakeQmpTransport>(qmp);
    auto  rspTransport = std::make_unique<FakeRspTransport>();
    auto* rsp          = rspTransport.get();
    rsp->registersXml  = x86RegisterXml();

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
    REQUIRE(controller.attach(7, invalidState, busy, error, unavailable).has_value());
    REQUIRE(controller
                .armWatch(7, {42, 0x5000, 4, mpfshost::DebugWatchAccess::Write}, invalidState, busy, error, unavailable)
                .has_value());

    std::uint32_t checks = 0;
    const auto    end    = controller.wait(
        {30000, 8},
        {},
        [&checks]()
        {
            return ++checks > 2;
        },
        7,
        invalidState,
        busy,
        error,
        unavailable);
    REQUIRE(end.has_value());
    REQUIRE(end->status == mpfshost::DebugWatchStatus::Cancelled);
    REQUIRE(rsp->watchRemoves == 0);

    // The wait left the guest running, although the stub continue does not update QMP's status flag.
    REQUIRE_FALSE(qmp->running);
    const auto detached = controller.detach(7, busy, error, unavailable);
    REQUIRE(detached.has_value());
    REQUIRE(detached->detached);
    REQUIRE(detached->guestResumed);
    REQUIRE(rsp->watchRemoves == 1);
    REQUIRE(qmp->running);
    REQUIRE_FALSE(qmp->stubActive);
}

TEST_CASE("resource requests require the owning attachment and a paused guest", "[core][debug][controller]")
{
    auto qmp                   = std::make_shared<FakeQmpState>();
    qmp->running               = true;
    auto qmpTransport          = std::make_unique<FakeQmpTransport>(qmp);
    auto rspTransport          = std::make_unique<FakeRspTransport>();
    rspTransport->registersXml = x86RegisterXml();

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

    const auto missing = controller.armBreakpoint(
        7, {42, 0x1000, mpfshost::DebugBreakpointKind::Software}, invalidState, busy, error, unavailable);
    REQUIRE_FALSE(missing.has_value());
    REQUIRE(invalidState);
    REQUIRE(error.find("attach") != std::string::npos);

    REQUIRE(controller.attach(7, invalidState, busy, error, unavailable).has_value());
    // Attach paused the guest, so arming is allowed.
    REQUIRE(controller
                .armWatch(7, {42, 0x5000, 4, mpfshost::DebugWatchAccess::Write}, invalidState, busy, error, unavailable)
                .has_value());

    // A different client bound to the same target shares the attachment.
    const auto shared = controller.armBreakpoint(8,
                                                 {42, 0xfffff80000123456ull, mpfshost::DebugBreakpointKind::Software},
                                                 invalidState,
                                                 busy,
                                                 error,
                                                 unavailable);
    REQUIRE(shared.has_value());

    // Further resources are refused only once the guest is running again.
    REQUIRE(controller.continueGuest(7, invalidState, busy, error, unavailable).has_value());
    invalidState = false;
    REQUIRE_FALSE(
        controller
            .armWatch(7, {42, 0x6000, 4, mpfshost::DebugWatchAccess::Write}, invalidState, busy, error, unavailable)
            .has_value());
    REQUIRE(invalidState);
    REQUIRE(error.find("pause") != std::string::npos);

    REQUIRE(controller.detach(7, busy, error, unavailable).has_value());
}

namespace
{
    std::unique_ptr<DebugController>
    makeBacktraceController(FakeInspector& inspector, std::uint64_t rip, std::uint64_t rsp, std::uint64_t rbp)
    {
        auto qmp                    = std::make_shared<FakeQmpState>();
        auto qmpTransport           = std::make_unique<FakeQmpTransport>(qmp);
        auto rspTransport           = std::make_unique<FakeRspTransport>();
        rspTransport->registersXml  = x86RegisterXml();
        rspTransport->registerBytes = registerBytes(rip, rsp, 0x1a2b3000ull, rbp);
        return std::make_unique<DebugController>(
            "/tmp/qmp.sock",
            "/tmp/stub.sock",
            [transport = std::move(qmpTransport)]() mutable
            {
                return std::move(transport);
            },
            [transport = std::move(rspTransport)]() mutable
            {
                return std::move(transport);
            },
            &inspector);
    }

    bool readQword(std::uint64_t, std::uint32_t size, std::vector<std::byte>& out, std::uint64_t value)
    {
        if (size != 8)
        {
            return false;
        }
        out.clear();
        out.reserve(size);
        for (std::uint32_t index = 0; index < size; ++index)
        {
            out.push_back(static_cast<std::byte>((value >> (index * 8)) & 0xFF));
        }
        return true;
    }
} // namespace

TEST_CASE("the backtrace walks a valid frame chain and stops at a null pointer", "[core][debug][controller]")
{
    FakeInspector inspector;
    inspector.module        = {0x400000, 0x10000, 0x10, "game.exe"};
    inspector.memoryHandler = [](std::uint64_t address, std::uint32_t size, std::vector<std::byte>& out, std::string&)
    {
        if (address == 0x1010)
        {
            return readQword(address, size, out, 0x1030);
        }
        if (address == 0x1018)
        {
            return readQword(address, size, out, 0x400100);
        }
        if (address == 0x1030)
        {
            return readQword(address, size, out, 0);
        }
        if (address == 0x1038)
        {
            return readQword(address, size, out, 0x400200);
        }
        return false;
    };

    auto        controller   = makeBacktraceController(inspector, 0x400010, 0x1000, 0x1010);
    bool        invalidState = false;
    bool        busy         = false;
    bool        unavailable  = false;
    std::string error;

    const auto response = controller->backtrace({42, 0, {}, 8}, invalidState, busy, error, unavailable);
    REQUIRE(response.has_value());
    REQUIRE(error.empty());
    REQUIRE_FALSE(invalidState);
    REQUIRE_FALSE(response->truncated);
    REQUIRE(response->frames.size() == 3);
    REQUIRE(response->frames[0].address == 0x400010);
    REQUIRE(response->frames[0].framePointer == 0x1010);
    REQUIRE(response->frames[1].address == 0x400100);
    REQUIRE(response->frames[1].framePointer == 0x1030);
    REQUIRE(response->frames[2].address == 0x400200);
    REQUIRE(response->frames[2].framePointer == 0);
    REQUIRE(response->frames[0].module == "game.exe");
}

TEST_CASE("the backtrace does not follow a corrupt frame pointer", "[core][debug][controller]")
{
    FakeInspector inspector;
    inspector.memoryHandler = [](std::uint64_t address, std::uint32_t size, std::vector<std::byte>& out, std::string&)
    {
        if (address == 0x1010)
        {
            return readQword(address, size, out, 0x1000); // does not advance past the current frame
        }
        if (address == 0x1018)
        {
            return readQword(address, size, out, 0x400100);
        }
        return false;
    };

    auto        controller   = makeBacktraceController(inspector, 0x400010, 0x1000, 0x1010);
    bool        invalidState = false;
    bool        busy         = false;
    bool        unavailable  = false;
    std::string error;

    const auto response = controller->backtrace({42, 0, {}, 8}, invalidState, busy, error, unavailable);
    REQUIRE(response.has_value());
    REQUIRE(response->frames.size() == 1);
    REQUIRE(response->truncated);
    REQUIRE(response->stopReason == "the saved frame pointer did not advance");

    inspector.memoryHandler = [](std::uint64_t, std::uint32_t, std::vector<std::byte>&, std::string&)
    {
        return false;
    };
    controller            = makeBacktraceController(inspector, 0x400010, 0x1000, 0x1010);
    const auto unreadable = controller->backtrace({42, 0, {}, 8}, invalidState, busy, error, unavailable);
    REQUIRE(unreadable.has_value());
    REQUIRE(unreadable->frames.size() == 1);
    REQUIRE(unreadable->truncated);
    REQUIRE(unreadable->stopReason.find("could not be read") != std::string::npos);
}

TEST_CASE("a backtrace frame keeps its raw address when symbolization fails", "[core][debug][controller]")
{
    FakeInspector inspector;
    inspector.hasModule     = false;
    inspector.memoryHandler = [](std::uint64_t address, std::uint32_t size, std::vector<std::byte>& out, std::string&)
    {
        if (address == 0x1018)
        {
            return readQword(address, size, out, 0);
        }
        return false;
    };

    auto        controller   = makeBacktraceController(inspector, 0x400010, 0x1000, 0x1010);
    bool        invalidState = false;
    bool        busy         = false;
    bool        unavailable  = false;
    std::string error;

    const auto response = controller->backtrace({42, 0, {}, 2}, invalidState, busy, error, unavailable);
    REQUIRE(response.has_value());
    REQUIRE(response->frames.size() == 1);
    REQUIRE(response->frames[0].address == 0x400010);
    REQUIRE(response->frames[0].module.empty());
    REQUIRE_FALSE(response->frames[0].exportFound);
    REQUIRE_FALSE(response->frames[0].pdbFound);
}
