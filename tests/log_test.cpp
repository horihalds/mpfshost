#include <catch2/catch.hpp>

#include "core/log/log.hpp"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace
{
    class LogGuard
    {
    public:
        explicit LogGuard(mpfshost::log::Level level = mpfshost::log::level()) : _previous {mpfshost::log::level()}
        {
            mpfshost::log::setLevel(level);
        }

        ~LogGuard()
        {
            mpfshost::log::setStderrSink();
            mpfshost::log::setLevel(_previous);
        }

        LogGuard(const LogGuard&)            = delete;
        LogGuard& operator=(const LogGuard&) = delete;

    private:
        mpfshost::log::Level _previous;
    };

    class StderrRedirect
    {
    public:
        StderrRedirect()
        {
            _saved = ::dup(STDERR_FILENO);
            REQUIRE(_saved >= 0);

            int fds[2] {};
            REQUIRE(::pipe(fds) == 0);
            _readFd = fds[0];
            REQUIRE(::dup2(fds[1], STDERR_FILENO) >= 0);
            ::close(fds[1]);
        }

        ~StderrRedirect()
        {
            restore();
            if (_readFd >= 0)
            {
                ::close(_readFd);
            }
        }

        StderrRedirect(const StderrRedirect&)            = delete;
        StderrRedirect& operator=(const StderrRedirect&) = delete;

        void restore()
        {
            if (_saved < 0)
            {
                return;
            }
            std::fflush(stderr);
            ::dup2(_saved, STDERR_FILENO);
            ::close(_saved);
            _saved = -1;
        }

        [[nodiscard]] std::string readAll()
        {
            std::string text;
            char        buffer[512] {};
            while (true)
            {
                const auto received = ::read(_readFd, buffer, sizeof(buffer));
                if (received <= 0)
                {
                    break;
                }
                text.append(buffer, static_cast<std::size_t>(received));
            }
            return text;
        }

    private:
        int _saved {-1};
        int _readFd {-1};
    };

    class EnvironmentGuard
    {
    public:
        explicit EnvironmentGuard(const char* name) : _name {name}
        {
            const char* value = std::getenv(name);
            if (value != nullptr)
            {
                _previous = value;
                _wasSet   = true;
            }
        }

        ~EnvironmentGuard()
        {
            if (_wasSet)
            {
                ::setenv(_name.c_str(), _previous.c_str(), 1);
            }
            else
            {
                ::unsetenv(_name.c_str());
            }
        }

        EnvironmentGuard(const EnvironmentGuard&)            = delete;
        EnvironmentGuard& operator=(const EnvironmentGuard&) = delete;

    private:
        std::string _name;
        std::string _previous;
        bool        _wasSet {false};
    };

    using mpfshost::log::Level;
} // namespace

TEST_CASE("log levels parse case-insensitively and reject unknown text", "[log]")
{
    CHECK(mpfshost::log::parseLevel("error") == Level::Error);
    CHECK(mpfshost::log::parseLevel("warn") == Level::Warn);
    CHECK(mpfshost::log::parseLevel("info") == Level::Info);
    CHECK(mpfshost::log::parseLevel("debug") == Level::Debug);
    CHECK(mpfshost::log::parseLevel("DEBUG") == Level::Debug);
    CHECK(mpfshost::log::parseLevel("Warn") == Level::Warn);

    CHECK_FALSE(mpfshost::log::parseLevel("").has_value());
    CHECK_FALSE(mpfshost::log::parseLevel("verbose").has_value());
    CHECK_FALSE(mpfshost::log::parseLevel("warning").has_value());
}

TEST_CASE("log level names round trip", "[log]")
{
    CHECK(mpfshost::log::levelName(Level::Error) == "error");
    CHECK(mpfshost::log::levelName(Level::Warn) == "warn");
    CHECK(mpfshost::log::levelName(Level::Info) == "info");
    CHECK(mpfshost::log::levelName(Level::Debug) == "debug");

    for (const auto level : {Level::Error, Level::Warn, Level::Info, Level::Debug})
    {
        const auto parsed = mpfshost::log::parseLevel(mpfshost::log::levelName(level));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == level);
    }
}

TEST_CASE("the level filters records before the sink sees them", "[log]")
{
    const LogGuard guard {Level::Info};

    std::vector<mpfshost::log::Record> records;
    mpfshost::log::setSink(
        [&records](const mpfshost::log::Record& record)
        {
            records.push_back(record);
        });

    CHECK(mpfshost::log::enabled(Level::Error));
    CHECK(mpfshost::log::enabled(Level::Info));
    CHECK_FALSE(mpfshost::log::enabled(Level::Debug));

    mpfshost::log::debug("session", "hidden ", 1);
    REQUIRE(records.empty());

    mpfshost::log::info("session", "shown");
    REQUIRE(records.size() == 1);
    CHECK(records[0].level == Level::Info);
    CHECK(records[0].category == "session");
    CHECK(records[0].message == "shown");

    mpfshost::log::setLevel(Level::Debug);
    mpfshost::log::debug("session", "visible ", 42);
    REQUIRE(records.size() == 2);
    CHECK(records[1].level == Level::Debug);
    CHECK(records[1].message == "visible 42");
}

TEST_CASE("a formatted record follows the documented layout", "[log]")
{
    mpfshost::log::Record record;
    record.level    = Level::Warn;
    record.category = "session";
    record.message  = "error response code=InvalidRange";
    record.time     = std::chrono::system_clock::time_point {std::chrono::milliseconds {123}};

    CHECK(mpfshost::log::format(record) == "1970-01-01T00:00:00.123Z warn  session: error response code=InvalidRange");
}

TEST_CASE("a formatted record can omit the timestamp for journald", "[log]")
{
    mpfshost::log::Record record;
    record.level    = Level::Warn;
    record.category = "session";
    record.message  = "error response code=InvalidRange";
    record.time     = std::chrono::system_clock::time_point {std::chrono::milliseconds {123}};

    CHECK(mpfshost::log::format(record, mpfshost::log::Timestamp::Omit)
          == "warn  session: error response code=InvalidRange");
}

TEST_CASE("the level field is padded to five characters", "[log]")
{
    mpfshost::log::Record record;
    record.category = "server";
    record.message  = "text";
    record.time     = std::chrono::system_clock::time_point {std::chrono::milliseconds {0}};

    record.level = Level::Error;
    CHECK(mpfshost::log::format(record) == "1970-01-01T00:00:00.000Z error server: text");

    record.level = Level::Info;
    CHECK(mpfshost::log::format(record) == "1970-01-01T00:00:00.000Z info  server: text");

    record.level = Level::Debug;
    CHECK(mpfshost::log::format(record) == "1970-01-01T00:00:00.000Z debug server: text");
}

TEST_CASE("an empty message still produces a well-formed line", "[log]")
{
    mpfshost::log::Record record;
    record.level    = Level::Info;
    record.category = "server";
    record.message  = "";
    record.time     = std::chrono::system_clock::time_point {std::chrono::milliseconds {0}};

    const auto line = mpfshost::log::format(record);
    CHECK(line == "1970-01-01T00:00:00.000Z info  server: ");
    CHECK_FALSE(line.ends_with('\n'));
}

TEST_CASE("a custom sink receives the record fields", "[log]")
{
    const LogGuard guard {Level::Debug};

    std::vector<mpfshost::log::Record> records;
    mpfshost::log::setSink(
        [&records](const mpfshost::log::Record& record)
        {
            records.push_back(record);
        });

    mpfshost::log::warn("host", "failed ", 7);

    REQUIRE(records.size() == 1);
    CHECK(records[0].level == Level::Warn);
    CHECK(records[0].category == "host");
    CHECK(records[0].message == "failed 7");
    CHECK(records[0].thread == std::this_thread::get_id());
    CHECK(records[0].time != std::chrono::system_clock::time_point {});
}

TEST_CASE("the default sink writes one complete line to stderr", "[log]")
{
    const LogGuard guard {Level::Info};

    StderrRedirect redirect;
    mpfshost::log::setStderrSink();
    mpfshost::log::info("sink", "hello");
    redirect.restore();

    const auto text = redirect.readAll();
    CHECK(text.ends_with("\n"));
    CHECK(text.find(" info  sink: hello\n") != std::string::npos);
    CHECK(text.find('\n') == text.size() - 1);
}

TEST_CASE("the default sink omits the timestamp when journald owns stderr", "[log]")
{
    const LogGuard   guard {Level::Info};
    EnvironmentGuard environment {"JOURNAL_STREAM"};
    StderrRedirect   redirect;

    struct stat info {};
    REQUIRE(::fstat(STDERR_FILENO, &info) == 0);
    const auto stream = std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino);
    REQUIRE(::setenv("JOURNAL_STREAM", stream.c_str(), 1) == 0);

    mpfshost::log::setStderrSink();
    mpfshost::log::info("sink", "hello");
    redirect.restore();

    CHECK(redirect.readAll() == "info  sink: hello\n");
}

TEST_CASE("concurrent records never interleave within a message", "[log]")
{
    const LogGuard guard {Level::Info};

    std::vector<mpfshost::log::Record> records;
    mpfshost::log::setSink(
        [&records](const mpfshost::log::Record& record)
        {
            records.push_back(record);
        });

    constexpr int kThreadCount = 8;
    constexpr int kPerThread   = 64;

    {
        std::vector<std::jthread> threads;
        threads.reserve(kThreadCount);
        for (int thread = 0; thread < kThreadCount; ++thread)
        {
            threads.emplace_back(
                [thread]
                {
                    for (int index = 0; index < kPerThread; ++index)
                    {
                        mpfshost::log::info("concurrent", "thread=", thread, " index=", index);
                    }
                });
        }
    }

    REQUIRE(records.size() == static_cast<std::size_t>(kThreadCount * kPerThread));
    for (const auto& record : records)
    {
        const auto marker = record.message.find(" index=");
        REQUIRE(marker != std::string::npos);
        const auto threadPart =
            record.message.substr(std::string {"thread="}.size(), marker - std::string {"thread="}.size());
        const auto indexPart = record.message.substr(marker + std::string {" index="}.size());
        CHECK(threadPart.find(' ') == std::string::npos);
        CHECK_FALSE(threadPart.empty());
        CHECK_FALSE(indexPart.empty());
    }
}
