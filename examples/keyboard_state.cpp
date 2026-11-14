#include <mpfshost/client.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace
{
    // Only the common keys are named here; the client API reports Windows virtual-key codes, so an application
    // can keep its own table or print the code directly.
    const char* keyName(std::uint8_t virtualKey)
    {
        switch (virtualKey)
        {
        case 0x08:
            return "backspace";
        case 0x09:
            return "tab";
        case 0x0D:
            return "return";
        case 0x10:
            return "shift";
        case 0x11:
            return "ctrl";
        case 0x12:
            return "alt";
        case 0x1B:
            return "esc";
        case 0x20:
            return "space";
        case 0x25:
            return "left";
        case 0x26:
            return "up";
        case 0x27:
            return "right";
        case 0x28:
            return "down";
        default:
            return nullptr;
        }
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: " << argv[0] << " <socket> <vm> [session]\n";
        return 2;
    }

    std::uint32_t sessionId = 0;
    if (argc > 3)
    {
        sessionId = static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 0));
    }

    const std::filesystem::path socketPath {argv[1]};
    mpfshost::Client            client {socketPath};

    auto session = client.bind(argv[2]);
    if (!session)
    {
        std::cerr << "bind: " << session.error().message << '\n';
        return 1;
    }

    // Reading the keyboard state needs Windows 11 22H2 or newer. It touches only guest memory, so the guest may
    // be running and no debugger attachment is involved.
    const auto state = session->keyboardState(sessionId);
    if (!state)
    {
        std::cerr << "keyboard state: " << state.error().message << '\n';
        return 1;
    }

    std::cout << "session=" << state->sessionId << " proxy-pid=" << state->proxyPid << " build=" << state->windowsBuild
              << " module=" << state->moduleName << '\n';

    if (state->pressedKeys.empty())
    {
        std::cout << "no keys are down\n";
        return 0;
    }

    std::cout << "pressed:";
    for (const auto virtualKey : state->pressedKeys)
    {
        const auto* name = keyName(virtualKey);
        std::cout << " 0x" << std::hex << static_cast<unsigned>(virtualKey) << std::dec;
        if (name != nullptr)
        {
            std::cout << '(' << name << ')';
        }
        else if (virtualKey >= 0x41 && virtualKey <= 0x5A)
        {
            std::cout << '(' << static_cast<char>('a' + (virtualKey - 0x41)) << ')';
        }
        else if (virtualKey >= 0x30 && virtualKey <= 0x39)
        {
            std::cout << '(' << static_cast<char>('0' + (virtualKey - 0x30)) << ')';
        }
    }
    std::cout << '\n';
    return 0;
}
