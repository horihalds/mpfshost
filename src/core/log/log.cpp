#include "core/log/log.hpp"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

#include <sys/stat.h>
#include <unistd.h>

namespace mpfshost::log
{
    namespace
    {
        std::atomic<Level> g_level {Level::Info};
        std::mutex         g_sinkMutex;

        // systemd sets JOURNAL_STREAM to "<device>:<inode>" when it captures stderr, and journald adds its own
        // timestamp to every entry. Compare it with the real stderr to leave the timestamp out only in that case.
        bool journalOwnsStderr()
        {
            const char* stream = std::getenv("JOURNAL_STREAM");
            if (stream == nullptr)
            {
                return false;
            }

            char*                    end    = nullptr;
            const unsigned long long device = std::strtoull(stream, &end, 10);
            if (end == stream || *end != ':')
            {
                return false;
            }
            const char*              inodeText = end + 1;
            const unsigned long long inode     = std::strtoull(inodeText, &end, 10);
            if (end == inodeText || *end != '\0')
            {
                return false;
            }

            struct stat info {};
            if (::fstat(STDERR_FILENO, &info) != 0)
            {
                return false;
            }
            return static_cast<unsigned long long>(info.st_dev) == device
                && static_cast<unsigned long long>(info.st_ino) == inode;
        }

        void writeStderr(const Record& record)
        {
            const auto line = format(record, journalOwnsStderr() ? Timestamp::Omit : Timestamp::Include);
            std::fwrite(line.data(), 1, line.size(), stderr);
            std::fputc('\n', stderr);
            std::fflush(stderr);
        }

        Sink g_sink {writeStderr};

        bool equalsIgnoreCase(std::string_view left, std::string_view right)
        {
            if (left.size() != right.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < left.size(); ++index)
            {
                const auto lhs = static_cast<unsigned char>(left[index]);
                const auto rhs = static_cast<unsigned char>(right[index]);
                if (std::tolower(lhs) != std::tolower(rhs))
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    std::optional<Level> parseLevel(std::string_view text)
    {
        if (equalsIgnoreCase(text, "error"))
        {
            return Level::Error;
        }
        if (equalsIgnoreCase(text, "warn"))
        {
            return Level::Warn;
        }
        if (equalsIgnoreCase(text, "info"))
        {
            return Level::Info;
        }
        if (equalsIgnoreCase(text, "debug"))
        {
            return Level::Debug;
        }
        return std::nullopt;
    }

    std::string_view levelName(Level level) noexcept
    {
        switch (level)
        {
        case Level::Error:
            return "error";
        case Level::Warn:
            return "warn";
        case Level::Info:
            return "info";
        case Level::Debug:
            return "debug";
        }
        return "info";
    }

    std::string format(const Record& record)
    {
        return format(record, Timestamp::Include);
    }

    std::string format(const Record& record, Timestamp timestamp)
    {
        const auto seconds = std::chrono::system_clock::to_time_t(record.time);
        std::tm    utc {};
        gmtime_r(&seconds, &utc);

        const auto milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(record.time.time_since_epoch()) % 1000;

        char stamp[128] {};
        std::snprintf(stamp,
                      sizeof(stamp),
                      "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                      utc.tm_year + 1900,
                      utc.tm_mon + 1,
                      utc.tm_mday,
                      utc.tm_hour,
                      utc.tm_min,
                      utc.tm_sec,
                      static_cast<int>(milliseconds.count()));

        const auto name = levelName(record.level);

        std::string line;
        line.reserve(std::strlen(stamp) + 1 + 5 + 1 + record.category.size() + 2 + record.message.size());
        if (timestamp == Timestamp::Include)
        {
            line.append(stamp);
            line.push_back(' ');
        }
        line.append(name);
        line.append(5 - name.size(), ' ');
        line.push_back(' ');
        line.append(record.category);
        line.append(": ");
        line.append(record.message);
        return line;
    }

    void setLevel(Level level) noexcept
    {
        g_level.store(level);
    }

    Level level() noexcept
    {
        return g_level.load();
    }

    bool enabled(Level level) noexcept
    {
        return level <= g_level.load();
    }

    void setSink(Sink sink)
    {
        std::lock_guard lock {g_sinkMutex};
        g_sink = sink ? std::move(sink) : Sink {writeStderr};
    }

    void setStderrSink()
    {
        setSink(Sink {writeStderr});
    }

    namespace detail
    {
        void emit(Level level, std::string_view category, std::string message)
        {
            Record record;
            record.level    = level;
            record.category = category;
            record.message  = std::move(message);
            record.time     = std::chrono::system_clock::now();
            record.thread   = std::this_thread::get_id();

            std::lock_guard lock {g_sinkMutex};
            if (g_sink)
            {
                g_sink(record);
            }
        }

        void append(std::string& out, std::string_view value)
        {
            out.append(value);
        }

        void append(std::string& out, const std::string& value)
        {
            out.append(value);
        }
    } // namespace detail
} // namespace mpfshost::log
