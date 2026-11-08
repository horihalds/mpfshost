#include <catch2/catch.hpp>

#include "core/debug/qmp_client.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using namespace std::chrono_literals;

    class FakeQmpTransport : public mpfshost::debug::QmpTransport
    {
    public:
        std::vector<std::string> incoming;
        std::vector<std::string> outgoing;
        std::filesystem::path    openedPath;
        bool                     openResult {true};
        bool                     writeResult {true};
        bool                     readTimesOut {false};
        bool                     readClosed {false};
        bool                     closed {false};
        std::string              openError;
        std::string              writeError;

        [[nodiscard]] bool
        open(const std::filesystem::path& socketPath, std::chrono::milliseconds, std::string& error) override
        {
            openedPath = socketPath;
            if (!openResult)
            {
                error = openError;
                return false;
            }
            return true;
        }

        [[nodiscard]] bool writeLine(std::string_view line, std::chrono::milliseconds, std::string& error) override
        {
            if (!writeResult)
            {
                error = writeError;
                return false;
            }
            outgoing.emplace_back(line);
            return true;
        }

        [[nodiscard]] bool
        readLine(std::string& line, std::chrono::milliseconds, bool& wasClosed, std::string& error) override
        {
            if (!incoming.empty())
            {
                line = std::move(incoming.front());
                incoming.erase(incoming.begin());
                return true;
            }
            if (readTimesOut)
            {
                error = "the QMP socket timed out";
                return false;
            }
            if (readClosed)
            {
                wasClosed = true;
                error     = "the QMP connection closed";
                return false;
            }
            error = "the fake has no scripted line";
            return false;
        }

        void close() noexcept override
        {
            closed = true;
        }
    };

    struct FakeQmp
    {
        FakeQmpTransport*                           raw {nullptr};
        std::unique_ptr<mpfshost::debug::QmpClient> client;

        explicit FakeQmp(std::vector<std::string> lines)
        {
            auto transport = std::make_unique<FakeQmpTransport>();
            raw            = transport.get();
            raw->incoming  = std::move(lines);
            client         = std::make_unique<mpfshost::debug::QmpClient>(std::move(transport));
        }
    };

    const std::string kGreeting     = R"({"QMP":{"version":{"qemu":{"major":10,"minor":2,"micro":2}}}})";
    const std::string kCapabilities = R"({"return":{}})";

    std::vector<std::string> handshake()
    {
        return {kGreeting, kCapabilities};
    }
} // namespace

TEST_CASE("QMP client negotiates capabilities after the greeting", "[core][debug][qmp]")
{
    FakeQmp qmp {handshake()};

    std::string error;
    REQUIRE(qmp.client->connect("/run/qmp.sock", 100ms, error));
    REQUIRE(qmp.raw->openedPath == "/run/qmp.sock");
    REQUIRE(qmp.raw->outgoing.size() == 1);
    REQUIRE(qmp.raw->outgoing.front().find("qmp_capabilities") != std::string::npos);
}

TEST_CASE("QMP client rejects a missing or malformed greeting", "[core][debug][qmp]")
{
    SECTION("return without a greeting")
    {
        FakeQmp     qmp {std::vector<std::string> {kCapabilities}};
        std::string error;
        REQUIRE_FALSE(qmp.client->connect("/run/qmp.sock", 100ms, error));
        REQUIRE(error.find("greeting") != std::string::npos);
    }

    SECTION("invalid JSON")
    {
        FakeQmp     qmp {{"not json"}};
        std::string error;
        REQUIRE_FALSE(qmp.client->connect("/run/qmp.sock", 100ms, error));
        REQUIRE(error.find("valid JSON") != std::string::npos);
    }
}

TEST_CASE("QMP client skips asynchronous events while matching the reply", "[core][debug][qmp]")
{
    auto lines = handshake();
    lines.emplace_back(R"({"event":"RESUME","data":{}})");
    lines.emplace_back(R"({"event":"STOP","data":{}})");
    lines.emplace_back(R"({"return":{"status":"running","running":true}})");

    FakeQmp     qmp {std::move(lines)};
    std::string error;
    REQUIRE(qmp.client->connect("/run/qmp.sock", 100ms, error));

    mpfshost::debug::QmpStatusInfo status;
    REQUIRE(qmp.client->queryStatus(status, 100ms, error));
    REQUIRE(status.status == "running");
    REQUIRE(status.running);
}

TEST_CASE("QMP client reports a QMP error reply", "[core][debug][qmp]")
{
    auto lines = handshake();
    lines.emplace_back(R"({"error":{"class":"GenericError","desc":"Runstate is running"}})");

    FakeQmp     qmp {std::move(lines)};
    std::string error;
    REQUIRE(qmp.client->connect("/run/qmp.sock", 100ms, error));

    std::string result;
    REQUIRE_FALSE(qmp.client->execute("cont", result, 100ms, error));
    REQUIRE(error.find("Runstate is running") != std::string::npos);
}

TEST_CASE("QMP client rejects a malformed reply line", "[core][debug][qmp]")
{
    auto lines = handshake();
    lines.emplace_back("this is not json");

    FakeQmp     qmp {std::move(lines)};
    std::string error;
    REQUIRE(qmp.client->connect("/run/qmp.sock", 100ms, error));

    std::string result;
    REQUIRE_FALSE(qmp.client->execute("query-status", result, 100ms, error));
    REQUIRE(error.find("malformed JSON") != std::string::npos);
}

TEST_CASE("QMP client bounds the wait for a reply", "[core][debug][qmp]")
{
    FakeQmp     qmp {handshake()};
    std::string error;
    REQUIRE(qmp.client->connect("/run/qmp.sock", 100ms, error));

    qmp.raw->readTimesOut = true;
    std::string result;
    REQUIRE_FALSE(qmp.client->execute("stop", result, 100ms, error));
    REQUIRE(error.find("timed out") != std::string::npos);
}

TEST_CASE("QMP client reports a closed control channel", "[core][debug][qmp]")
{
    FakeQmp     qmp {handshake()};
    std::string error;
    REQUIRE(qmp.client->connect("/run/qmp.sock", 100ms, error));

    qmp.raw->readClosed = true;
    std::string result;
    REQUIRE_FALSE(qmp.client->execute("stop", result, 100ms, error));
    REQUIRE(error.find("closed") != std::string::npos);
}

TEST_CASE("QMP client reads the vCPU count from query-cpus-fast", "[core][debug][qmp]")
{
    auto lines = handshake();
    lines.emplace_back(R"({"return":[{"cpu-index":0},{"cpu-index":1},{"cpu-index":2}]})");

    FakeQmp     qmp {std::move(lines)};
    std::string error;
    REQUIRE(qmp.client->connect("/run/qmp.sock", 100ms, error));

    std::uint32_t count = 0;
    REQUIRE(qmp.client->queryVcpuCount(count, 100ms, error));
    REQUIRE(count == 3);
}

TEST_CASE("QMP run state mapping is stable", "[core][debug][qmp]")
{
    REQUIRE(mpfshost::debug::qmpRunState("running") == mpfshost::DebugRunState::Running);
    REQUIRE(mpfshost::debug::qmpRunState("paused") == mpfshost::DebugRunState::Paused);
    REQUIRE(mpfshost::debug::qmpRunState("shutdown") == mpfshost::DebugRunState::Shutdown);
    REQUIRE(mpfshost::debug::qmpRunState("suspended") == mpfshost::DebugRunState::Suspended);
    REQUIRE(mpfshost::debug::qmpRunState("guest-panicked") == mpfshost::DebugRunState::GuestPanicked);
    REQUIRE(mpfshost::debug::qmpRunState("") == mpfshost::DebugRunState::Unknown);
    REQUIRE(mpfshost::debug::qmpRunState("inmigrate") == mpfshost::DebugRunState::Other);
}
