#include "core/client/client.hpp"

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: " << argv[0] << " <socket> <vm> [path]\n";
        return 2;
    }

    const std::filesystem::path socketPath {argv[1]};
    std::string                 error;

    mpfshost::VfsQuery query;
    query.targetName = argv[2];
    if (argc > 3)
    {
        query.path = argv[3];
    }

    const auto entries = mpfshost::listVfs(socketPath, query, error);
    if (!entries)
    {
        std::cerr << "list-vfs: " << error << '\n';
        return 1;
    }
    for (const auto& entry : *entries)
    {
        std::cout << (entry.isDirectory ? 'd' : 'f') << '\t' << entry.size << '\t' << entry.name << '\n';
    }
    return 0;
}
