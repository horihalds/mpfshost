#include <mpfshost/client.hpp>

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
    mpfshost::Client            client {socketPath};

    auto session = client.bind(argv[2]);
    if (!session)
    {
        std::cerr << "bind: " << session.error().message << '\n';
        return 1;
    }

    mpfshost::ProcessFilter filter;
    if (argc > 3)
    {
        filter.nameFilter = argv[3];
    }

    const auto processes = session->processes(filter);
    if (!processes)
    {
        std::cerr << "list-processes: " << processes.error().message << '\n';
        return 1;
    }
    for (const auto& process : *processes)
    {
        std::cout << process.pid << '\t' << process.parentPid << '\t' << process.name << '\n';
    }
    return 0;
}
