#include <mpfshost/client.hpp>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    void appendTap(std::vector<mpfshost::InputEvent>& events, const char* qcode)
    {
        mpfshost::InputEvent key;
        key.kind = mpfshost::InputEventKind::Key;
        key.key  = qcode;
        key.down = true;
        events.push_back(key);
        key.down = false;
        events.push_back(key);
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: " << argv[0] << " <socket> <vm> [--click]\n";
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

    // QEMU qcodes are the wire vocabulary; the CLI and MCP use the key-map helper to resolve names and text.
    std::vector<mpfshost::InputEvent> events;
    for (const char* qcode : {"h", "e", "l", "l", "o", "spc", "w", "o", "r", "l", "d", "ret"})
    {
        appendTap(events, qcode);
    }

    if (argc > 3 && std::string {argv[3]} == "--click")
    {
        mpfshost::InputEvent move;
        move.kind     = mpfshost::InputEventKind::PointerMove;
        move.absolute = false; // relative movement works with virtio-mouse
        move.x        = 10;
        move.y        = 10;
        events.push_back(move);

        mpfshost::InputEvent click;
        click.kind   = mpfshost::InputEventKind::PointerButton;
        click.button = mpfshost::InputMouseButton::Left;
        click.down   = true;
        events.push_back(click);
        click.down = false;
        events.push_back(click);
    }

    const auto sent = session->sendInput(events);
    if (!sent)
    {
        std::cerr << "input: " << sent.error().message << '\n';
        return 1;
    }
    std::cout << "sent " << sent->eventsSent << " events\n";
    return 0;
}
