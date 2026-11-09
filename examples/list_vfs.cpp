#include <mpfshost/client.hpp>

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
    mpfshost::Client            client {socketPath};

    auto session = client.bind(argv[2]);
    if (!session)
    {
        std::cerr << "bind: " << session.error().message << '\n';
        return 1;
    }

    const std::string path    = argc > 3 ? argv[3] : "/";
    const auto        entries = session->listVfs(path);
    if (!entries)
    {
        std::cerr << "list-vfs: " << entries.error().message << '\n';
        return 1;
    }
    for (const auto& entry : *entries)
    {
        std::cout << (entry.isDirectory ? 'd' : 'f') << '\t' << entry.size << '\t' << entry.name << '\n';
    }
    return 0;
}
