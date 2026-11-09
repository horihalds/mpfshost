#include <catch2/catch.hpp>

#include "core/debug/rsp_client.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using namespace std::chrono_literals;

    unsigned checksum(std::string_view data)
    {
        unsigned sum = 0;
        for (const char value : data)
        {
            sum += static_cast<unsigned char>(value);
        }
        return sum & 0xFF;
    }

    std::string packet(std::string_view body)
    {
        static constexpr char digits[] = "0123456789abcdef";
        const auto            sum      = checksum(body);
        std::string           text {"$"};
        text += body;
        text.push_back('#');
        text.push_back(digits[(sum >> 4) & 0x0F]);
        text.push_back(digits[sum & 0x0F]);
        return text;
    }

    std::string acked(std::string_view body)
    {
        return "+" + packet(body);
    }

    class FakeRspTransport : public mpfshost::debug::RspTransport
    {
    public:
        std::string              incoming;
        std::vector<std::string> written;
        bool                     openResult {true};
        bool                     closed {false};
        std::string              openError;

        [[nodiscard]] bool
        open(const mpfshost::debug::RspEndpoint&, std::chrono::milliseconds, std::string& error) override
        {
            if (!openResult)
            {
                error = openError;
                return false;
            }
            return true;
        }

        [[nodiscard]] std::ptrdiff_t
        readSome(std::span<std::byte> buffer, std::chrono::milliseconds, std::string& error) override
        {
            if (incoming.empty())
            {
                error = "the fake has no scripted bytes";
                return -1;
            }
            const auto size = std::min(buffer.size(), incoming.size());
            std::memcpy(buffer.data(), incoming.data(), size);
            incoming.erase(0, size);
            return static_cast<std::ptrdiff_t>(size);
        }

        [[nodiscard]] bool writeAll(std::span<const std::byte> data, std::chrono::milliseconds, std::string&) override
        {
            written.emplace_back(reinterpret_cast<const char*>(data.data()), data.size());
            return true;
        }

        void close() noexcept override
        {
            closed = true;
        }
    };

    std::vector<std::string> packetWrites(const FakeRspTransport& transport)
    {
        std::vector<std::string> packets;
        for (const auto& write : transport.written)
        {
            if (!write.empty() && write.front() == '$')
            {
                packets.push_back(write);
            }
        }
        return packets;
    }

    struct FakeRsp
    {
        FakeRspTransport*                           raw {nullptr};
        std::unique_ptr<mpfshost::debug::RspClient> client;

        explicit FakeRsp(std::string script)
        {
            auto transport = std::make_unique<FakeRspTransport>();
            raw            = transport.get();
            raw->incoming  = std::move(script);
            client         = std::make_unique<mpfshost::debug::RspClient>(std::move(transport));
        }
    };
} // namespace

TEST_CASE("RSP negotiates qSupported and enters no-ack mode when offered", "[core][debug][rsp]")
{
    FakeRsp     rsp {acked("PacketSize=20020;qXfer:features:read+;QStartNoAckMode+") + acked("OK")};
    std::string error;
    REQUIRE(rsp.client->negotiate(100ms, error));
    REQUIRE(rsp.client->noAck());
    const auto packets = packetWrites(*rsp.raw);
    REQUIRE(packets.size() == 2);
    REQUIRE(packets[0].find("$qSupported:multiprocess+;xmlRegisters=i386#") == 0);
    REQUIRE(packets[1].find("$QStartNoAckMode#") == 0);

    const auto reference = packet("qSupported:multiprocess+;xmlRegisters=i386");
    REQUIRE(packets[0] == reference);
}

TEST_CASE("RSP prefers vCont when the stub advertises it", "[core][debug][rsp]")
{
    FakeRsp     rsp {acked("PacketSize=20020;vContSupported+")};
    std::string error;
    REQUIRE(rsp.client->negotiate(100ms, error));
    REQUIRE(rsp.client->continueExecution(100ms, error));
    const auto packets = packetWrites(*rsp.raw);
    REQUIRE(packets.size() == 2);
    REQUIRE(packets[1].find("$vCont;c#") == 0);
}

TEST_CASE("RSP falls back to c when vCont is not advertised", "[core][debug][rsp]")
{
    FakeRsp     rsp {acked("PacketSize=1000;qXfer:features:read+")};
    std::string error;
    REQUIRE(rsp.client->negotiate(100ms, error));
    REQUIRE(rsp.client->continueExecution(100ms, error));
    const auto packets = packetWrites(*rsp.raw);
    REQUIRE(packets.size() == 2);
    REQUIRE(packets[1].find("$c#") == 0);
}

TEST_CASE("RSP formats breakpoint and step packets", "[core][debug][rsp]")
{
    FakeRsp     rsp {acked("OK") + acked("OK") + acked("OK") + acked("OK")};
    std::string error;
    REQUIRE(rsp.client->insertBreakpoint(0x401000, false, 100ms, error));
    REQUIRE(rsp.client->removeBreakpoint(0x401000, false, 100ms, error));
    REQUIRE(rsp.client->insertBreakpoint(0x401000, true, 100ms, error));
    REQUIRE(rsp.client->removeBreakpoint(0x401000, true, 100ms, error));

    const auto packets = packetWrites(*rsp.raw);
    REQUIRE(packets.size() == 4);
    REQUIRE(packets[0].find("$Z0,401000,1#") == 0);
    REQUIRE(packets[1].find("$z0,401000,1#") == 0);
    REQUIRE(packets[2].find("$Z1,401000,1#") == 0);
    REQUIRE(packets[3].find("$z1,401000,1#") == 0);
}

TEST_CASE("RSP formats write, read and access watchpoint packets", "[core][debug][rsp]")
{
    FakeRsp     rsp {acked("OK") + acked("OK") + acked("OK") + acked("OK") + acked("OK") + acked("OK")};
    std::string error;
    bool        unsupported = false;
    REQUIRE(rsp.client->insertWatchpoint(0x401000, 4, mpfshost::DebugWatchAccess::Write, 100ms, error, unsupported));
    REQUIRE_FALSE(unsupported);
    REQUIRE(rsp.client->removeWatchpoint(0x401000, 4, mpfshost::DebugWatchAccess::Write, 100ms, error));
    REQUIRE(rsp.client->insertWatchpoint(0x401000, 8, mpfshost::DebugWatchAccess::Read, 100ms, error, unsupported));
    REQUIRE_FALSE(unsupported);
    REQUIRE(rsp.client->removeWatchpoint(0x401000, 8, mpfshost::DebugWatchAccess::Read, 100ms, error));
    REQUIRE(rsp.client->insertWatchpoint(0x401000, 1, mpfshost::DebugWatchAccess::Access, 100ms, error, unsupported));
    REQUIRE_FALSE(unsupported);
    REQUIRE(rsp.client->removeWatchpoint(0x401000, 1, mpfshost::DebugWatchAccess::Access, 100ms, error));

    const auto packets = packetWrites(*rsp.raw);
    REQUIRE(packets.size() == 6);
    REQUIRE(packets[0].find("$Z2,401000,4#") == 0);
    REQUIRE(packets[1].find("$z2,401000,4#") == 0);
    REQUIRE(packets[2].find("$Z3,401000,8#") == 0);
    REQUIRE(packets[3].find("$z3,401000,8#") == 0);
    REQUIRE(packets[4].find("$Z4,401000,1#") == 0);
    REQUIRE(packets[5].find("$z4,401000,1#") == 0);
}

TEST_CASE("RSP reports a read watchpoint the stub does not implement", "[core][debug][rsp]")
{
    FakeRsp     rsp {acked("")};
    std::string error;
    bool        unsupported = false;
    REQUIRE_FALSE(
        rsp.client->insertWatchpoint(0x401000, 4, mpfshost::DebugWatchAccess::Read, 100ms, error, unsupported));
    REQUIRE(unsupported);
    REQUIRE(error.find("read watchpoints") != std::string::npos);
}

TEST_CASE("RSP names the thread in a vCont single step", "[core][debug][rsp]")
{
    FakeRsp     rsp {acked("PacketSize=20020;vContSupported+")};
    std::string error;
    REQUIRE(rsp.client->negotiate(100ms, error));
    REQUIRE(rsp.client->stepExecution("p01.03", 100ms, error));
    const auto packets = packetWrites(*rsp.raw);
    REQUIRE(packets.size() == 2);
    REQUIRE(packets[1].find("$vCont;s:p01.03#") == 0);
}

TEST_CASE("RSP stays in ack mode when no-ack is not advertised", "[core][debug][rsp]")
{
    FakeRsp     rsp {acked("PacketSize=1000;qXfer:features:read+")};
    std::string error;
    REQUIRE(rsp.client->negotiate(100ms, error));
    REQUIRE_FALSE(rsp.client->noAck());
    REQUIRE(packetWrites(*rsp.raw).size() == 1);
}

TEST_CASE("RSP enumerates threads and selects one", "[core][debug][rsp]")
{
    FakeRsp                  rsp {acked("m1.1,1.2") + acked("m1.3") + acked("l") + acked("OK")};
    std::string              error;
    std::vector<std::string> threads;
    REQUIRE(rsp.client->queryThreads(threads, 100ms, error));
    REQUIRE(threads == std::vector<std::string> {"1.1", "1.2", "1.3"});
    REQUIRE(rsp.client->selectThread("1.2", 100ms, error));
    REQUIRE(packetWrites(*rsp.raw).back() == packet("Hg1.2"));
}

TEST_CASE("RSP fetches target.xml and follows its includes", "[core][debug][rsp]")
{
    const std::string target = R"(<target><xi:include href="i386-64bit.xml"/></target>)";
    const std::string features =
        R"(<feature><reg name="rax" bitsize="64" regnum="0"/><reg name="rip" bitsize="64"/></feature>)";
    FakeRsp rsp {acked("l" + target) + acked("l" + features)};

    std::string                           error;
    mpfshost::debug::RspTargetDescription description;
    REQUIRE(rsp.client->readTargetDescription(description, 100ms, error));
    REQUIRE(description.registers.size() == 2);
    REQUIRE(description.registers[0].name == "rax");
    REQUIRE(description.registers[0].number == 0);
    REQUIRE(description.registers[1].name == "rip");
    REQUIRE(description.registers[1].number == 1);
    REQUIRE(description.registers[1].offset == 8);
}

TEST_CASE("RSP register parsing honours regnum and ignores comments", "[core][debug][rsp]")
{
    const std::string xml         = R"(
        <feature>
          <reg name="rax" bitsize="64" regnum="0"/>
          <!-- <reg name="ignored" bitsize="64"/> -->
          <reg name="rip" bitsize="64"/>
          <reg name="eflags" bitsize="32"/>
          <reg name="cr3" bitsize="64" regnum="29"/>
          <reg name="cr4" bitsize="64"/>
        </feature>)";
    const auto        description = mpfshost::debug::parseRspTargetDescription(xml);
    REQUIRE(description.registers.size() == 5);
    REQUIRE(description.registers[1].name == "rip");
    REQUIRE(description.registers[1].number == 1);
    REQUIRE(description.registers[1].offset == 8);
    REQUIRE(description.registers[2].name == "eflags");
    REQUIRE(description.registers[2].offset == 16);
    REQUIRE(description.registers[3].name == "cr3");
    REQUIRE(description.registers[3].number == 29);
    REQUIRE(description.registers[3].offset == 20);
    REQUIRE(description.registers[4].name == "cr4");
    REQUIRE(description.registers[4].number == 30);
}

TEST_CASE("RSP register slicing reads little-endian values", "[core][debug][rsp]")
{
    const auto description = mpfshost::debug::parseRspTargetDescription(
        R"(<reg name="rax" bitsize="64"/><reg name="eflags" bitsize="32"/><reg name="cr3" bitsize="64"/>)");
    std::vector<std::byte> raw(20, std::byte {0});
    raw[0]  = std::byte {0x11};
    raw[7]  = std::byte {0x22};
    raw[8]  = std::byte {0x33};
    raw[11] = std::byte {0x44};
    raw[12] = std::byte {0x55};
    raw[15] = std::byte {0x66};

    const auto rax    = mpfshost::debug::findRspRegister(description, "rax");
    const auto eflags = mpfshost::debug::findRspRegister(description, "eflags");
    const auto cr3    = mpfshost::debug::findRspRegister(description, "cr3");
    REQUIRE(rax.has_value());
    REQUIRE(mpfshost::debug::readRspRegisterValue(raw, *rax) == 0x2200000000000011ull);
    REQUIRE(eflags.has_value());
    REQUIRE(mpfshost::debug::readRspRegisterValue(raw, *eflags) == 0x44000033ull);
    REQUIRE(cr3.has_value());
    REQUIRE(mpfshost::debug::readRspRegisterValue(raw, *cr3) == 0x66000055ull);
    REQUIRE_FALSE(mpfshost::debug::readRspRegisterValue(std::span<const std::byte>(raw).first(14), *cr3).has_value());
}

TEST_CASE("RSP retries a rejected packet", "[core][debug][rsp]")
{
    FakeRsp                  rsp {"-" + acked("l")};
    std::string              error;
    std::vector<std::string> threads;
    REQUIRE(rsp.client->queryThreads(threads, 100ms, error));
    REQUIRE(threads.empty());
    REQUIRE(packetWrites(*rsp.raw).size() == 2);
}

TEST_CASE("RSP rejects a bad checksum", "[core][debug][rsp]")
{
    FakeRsp                  rsp {"+" + std::string {"$OK#00"}};
    std::string              error;
    std::vector<std::string> threads;
    REQUIRE_FALSE(rsp.client->queryThreads(threads, 100ms, error));
    REQUIRE(error.find("checksum") != std::string::npos);
    REQUIRE(packetWrites(*rsp.raw).size() == 1);
}

TEST_CASE("RSP reads and decodes a register packet", "[core][debug][rsp]")
{
    const std::string hex = "1bab89aeffffffff";
    FakeRsp           rsp {acked(hex)};
    std::string       error;
    std::uint64_t     value = 0;
    REQUIRE(rsp.client->readRegister(29, value, 100ms, error));
    REQUIRE(value == 0xffffffffae89ab1bull);
    REQUIRE(packetWrites(*rsp.raw).front() == packet("p1d"));
}
