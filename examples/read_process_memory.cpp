#include "core/client/client.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 6)
    {
        std::cerr << "usage: " << argv[0] << " <socket> <vm> <pid> <address> <size>\n";
        return 2;
    }

    const std::filesystem::path socketPath {argv[1]};
    std::string                 error;

    mpfshost::MemoryQuery query;
    query.targetName = argv[2];
    query.pid        = static_cast<std::uint32_t>(std::stoul(argv[3]));
    query.address    = std::stoull(argv[4], nullptr, 0);
    query.size       = static_cast<std::uint32_t>(std::stoul(argv[5], nullptr, 0));

    const auto data = mpfshost::readMemory(socketPath, query, error);
    if (!data)
    {
        std::cerr << "read-memory: " << error << '\n';
        return 1;
    }

    constexpr std::size_t kBytesPerLine = 16;
    for (std::size_t offset = 0; offset < data->size(); offset += kBytesPerLine)
    {
        std::cout << std::hex << std::setw(16) << std::setfill('0') << query.address + offset << ':';
        const std::size_t end = std::min(offset + kBytesPerLine, data->size());
        for (std::size_t index = offset; index < end; ++index)
        {
            const auto value = std::to_integer<unsigned int>((*data)[index]);
            std::cout << ' ' << std::setw(2) << std::setfill('0') << value;
        }
        std::cout << std::dec << '\n';
    }
    return 0;
}
