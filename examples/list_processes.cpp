#include "core/client/client.hpp"

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: " << argv[0] << " <socket> <vm> [name-filter]\n";
        return 2;
    }

    const std::filesystem::path socketPath {argv[1]};
    std::string                 error;

    mpfshost::ProcessQuery query;
    query.targetName = argv[2];
    if (argc > 3)
    {
        query.nameFilter = argv[3];
    }

    const auto processes = mpfshost::listProcesses(socketPath, query, error);
    if (!processes)
    {
        std::cerr << "list-processes: " << error << '\n';
        return 1;
    }
    for (const auto& process : processes->processes)
    {
        std::cout << process.pid << '\t' << process.parentPid << '\t' << process.name << '\n';
    }
    return 0;
}
