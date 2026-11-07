#include "core/client/client.hpp"

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    const std::filesystem::path socketPath =
        argc > 1 ? std::filesystem::path {argv[1]} : std::filesystem::path {"/run/mpfshost.sock"};
    std::string error;

    const auto capabilities = mpfshost::getCapabilities(socketPath, error);
    if (!capabilities)
    {
        std::cerr << "capabilities: " << error << '\n';
        return 1;
    }
    std::cout << "host version: " << capabilities->serverVersion << '\n';
    std::cout << "request types: " << capabilities->requestTypes.size() << '\n';

    const auto targets = mpfshost::listTargets(socketPath, error);
    if (!targets)
    {
        std::cerr << "list-targets: " << error << '\n';
        return 1;
    }
    if (targets->targets.empty())
    {
        std::cout << "no targets\n";
        return 0;
    }
    for (const auto& target : targets->targets)
    {
        std::cout << target.name << " pid=" << target.qemuPid << " qmp=" << target.qmpSocketPath << '\n';
    }
    return 0;
}
