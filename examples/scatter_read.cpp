#include <mpfshost/client.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    mpfshost::ScatterRange parseRange(const std::string& text)
    {
        const auto separator = text.find(':');
        if (separator == std::string::npos)
        {
            throw std::invalid_argument {"range must be <address>:<size>"};
        }
        mpfshost::ScatterRange range;
        range.address = std::stoull(text.substr(0, separator), nullptr, 0);
        range.size    = static_cast<std::uint32_t>(std::stoul(text.substr(separator + 1), nullptr, 0));
        return range;
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc < 5)
    {
        std::cerr << "usage: " << argv[0] << " <socket> <vm> <pid> <address>:<size> [<address>:<size> ...]\n";
        return 2;
    }

    const std::filesystem::path socketPath {argv[1]};
    mpfshost::Client            client {socketPath};

    auto session = client.bind(argv[2]);
    if (!session)
    {
        std::cerr << "bind: " << session.error().message << '\n';
        return 1;
    }

    const auto                          pid = static_cast<std::uint32_t>(std::stoul(argv[3]));
    std::vector<mpfshost::ScatterRange> ranges;
    try
    {
        for (int index = 4; index < argc; ++index)
        {
            ranges.push_back(parseRange(argv[index]));
        }
    }
    catch (const std::exception& exception)
    {
        std::cerr << "invalid range: " << exception.what() << '\n';
        return 2;
    }

    const auto results = session->scatterRead(pid, ranges);
    if (!results)
    {
        std::cerr << "scatter-read: " << results.error().message << '\n';
        return 1;
    }

    for (const auto& result : *results)
    {
        std::cout << std::hex << result.address << std::dec << " requested=" << result.requestedSize
                  << " read=" << result.bytesRead << (result.ok ? " ok" : " failed") << '\n';
    }
    return 0;
}
