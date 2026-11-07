#include "core/target/scanner.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace mpfshost
{
    namespace
    {
        bool readPid(const std::filesystem::path& path, std::uint32_t& pid)
        {
            std::ifstream file {path};
            if (!file)
            {
                return false;
            }

            std::string content;
            std::getline(file, content);
            content.erase(content.find_last_not_of(" \t\r\n") + 1);
            if (content.empty())
            {
                return false;
            }

            const auto* begin  = content.data();
            const auto* end    = begin + content.size();
            const auto  result = std::from_chars(begin, end, pid);
            return result.ec == std::errc {} && result.ptr == end;
        }
    } // namespace

    TargetScanner::TargetScanner(std::filesystem::path pidDir, std::filesystem::path qmpDir)
        : _pidDir {std::move(pidDir)}, _qmpDir {std::move(qmpDir)}
    {
    }

    std::vector<VmTarget> TargetScanner::scan() const
    {
        std::vector<VmTarget> targets;

        std::error_code error;
        if (!std::filesystem::is_directory(_pidDir, error))
        {
            return targets;
        }
        if (!std::filesystem::is_directory(_qmpDir, error))
        {
            return targets;
        }

        const std::filesystem::directory_iterator end;
        std::filesystem::directory_iterator       iterator {_pidDir, error};
        while (!error && iterator != end)
        {
            const auto&     entry = *iterator;
            const auto&     path  = entry.path();
            std::error_code entryError;
            if (path.extension() == ".pid" && entry.is_regular_file(entryError))
            {
                const auto      name   = path.stem().string();
                auto            socket = _qmpDir / (name + ".sock");
                std::error_code socketError;
                if (std::filesystem::exists(socket, socketError))
                {
                    std::uint32_t pid {};
                    if (readPid(path, pid))
                    {
                        targets.push_back({name, pid, std::move(socket)});
                    }
                }
            }
            iterator.increment(error);
        }

        std::sort(targets.begin(),
                  targets.end(),
                  [](const VmTarget& lhs, const VmTarget& rhs)
                  {
                      return lhs.name < rhs.name;
                  });
        return targets;
    }
} // namespace mpfshost
