#ifndef MPFSHOST_CORE_LOG_LOG_HPP
#define MPFSHOST_CORE_LOG_LOG_HPP
#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace mpfshost::log
{
    enum class Level
    {
        Error = 0,
        Warn,
        Info,
        Debug
    };

    struct Record
    {
        Level                                 level {Level::Info};
        std::string_view                      category;
        std::string                           message;
        std::chrono::system_clock::time_point time {};
        std::thread::id                       thread {};
    };

    using Sink = std::function<void(const Record&)>;

    [[nodiscard]] std::optional<Level> parseLevel(std::string_view text);
    [[nodiscard]] std::string_view     levelName(Level level) noexcept;
    [[nodiscard]] std::string          format(const Record& record); // one line, no trailing newline

    void                setLevel(Level level) noexcept;
    [[nodiscard]] Level level() noexcept;
    [[nodiscard]] bool  enabled(Level level) noexcept;

    void setSink(Sink sink);
    void setStderrSink(); // restores the default sink

    namespace detail
    {
        void emit(Level level, std::string_view category, std::string message);

        void append(std::string& out, std::string_view value);
        void append(std::string& out, const std::string& value);

        inline void append(std::string& out, const char* value)
        {
            out.append(value);
        }

        template<typename T>
            requires std::is_integral_v<std::remove_cvref_t<T>>
        void append(std::string& out, T value)
        {
            out += std::to_string(value);
        }
    } // namespace detail

    template<typename... Args>
    void write(Level level, std::string_view category, Args&&... args)
    {
        if (!enabled(level))
        {
            return;
        }
        std::string message;
        (detail::append(message, std::forward<Args>(args)), ...);
        detail::emit(level, category, std::move(message));
    }

    template<typename... Args>
    void error(std::string_view category, Args&&... args)
    {
        write(Level::Error, category, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void warn(std::string_view category, Args&&... args)
    {
        write(Level::Warn, category, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void info(std::string_view category, Args&&... args)
    {
        write(Level::Info, category, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void debug(std::string_view category, Args&&... args)
    {
        write(Level::Debug, category, std::forward<Args>(args)...);
    }
} // namespace mpfshost::log

#endif // MPFSHOST_CORE_LOG_LOG_HPP
