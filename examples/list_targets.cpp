#include <mpfshost/client.hpp>

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    const std::filesystem::path socketPath =
        argc > 1 ? std::filesystem::path {argv[1]} : std::filesystem::path {mpfshost::kDefaultSocketPath};
    mpfshost::Client client {socketPath};

    const auto capabilities = client.capabilities();
    if (!capabilities)
    {
        std::cerr << "capabilities: " << capabilities.error().message << '\n';
        return 1;
    }
    std::cout << "host version: " << capabilities->serverVersion << '\n';
    std::cout << "request types: " << capabilities->requestTypes.size() << '\n';

    const auto targets = client.targets();
    if (!targets)
    {
        std::cerr << "list-targets: " << targets.error().message << '\n';
        return 1;
    }
    if (targets->empty())
    {
        std::cout << "no targets\n";
        return 0;
    }
    for (const auto& target : *targets)
    {
        std::cout << target.name << " pid=" << target.qemuPid << " qmp=" << target.qmpSocketPath << '\n';
    }
    return 0;
}
