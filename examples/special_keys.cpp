#include <mpfshost/client.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    struct NamedKey
    {
        std::string_view name;
        std::string_view qcode;
    };

    // Friendly names for the keys that have no printable character. A name that is not listed is passed through
    // as a QEMU qcode, so `pgup` and `page-up` both work. The full QKeyCode list also covers a, b, 0..9 and the
    // punctuation names such as minus, equal, comma, dot and slash.
    constexpr NamedKey kSpecialKeys[] = {
        {    "page-up",        "pgup"},
        {  "page-down",        "pgdn"},
        {     "pageup",        "pgup"},
        {   "pagedown",        "pgdn"},
        {       "pgup",        "pgup"},
        {       "pgdn",        "pgdn"},
        {       "home",        "home"},
        {        "end",         "end"},
        {     "insert",      "insert"},
        {        "ins",      "insert"},
        {     "delete",      "delete"},
        {        "del",      "delete"},
        {         "up",          "up"},
        {       "down",        "down"},
        {       "left",        "left"},
        {      "right",       "right"},
        {    "arrowup",          "up"},
        {  "arrowdown",        "down"},
        {  "arrowleft",        "left"},
        { "arrowright",       "right"},
        {     "escape",         "esc"},
        {        "esc",         "esc"},
        {        "tab",         "tab"},
        {  "backspace",   "backspace"},
        {      "enter",         "ret"},
        {     "return",         "ret"},
        {      "space",         "spc"},
        {  "caps-lock",   "caps_lock"},
        {   "num-lock",    "num_lock"},
        {"scroll-lock", "scroll_lock"},
        {       "menu",        "menu"},
        {      "print",       "print"},
        {         "f1",          "f1"},
        {         "f2",          "f2"},
        {         "f3",          "f3"},
        {         "f4",          "f4"},
        {         "f5",          "f5"},
        {         "f6",          "f6"},
        {         "f7",          "f7"},
        {         "f8",          "f8"},
        {         "f9",          "f9"},
        {        "f10",         "f10"},
        {        "f11",         "f11"},
        {        "f12",         "f12"},
    };

    std::string lower(std::string_view text)
    {
        std::string result;
        result.reserve(text.size());
        for (const char character : text)
        {
            result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        }
        return result;
    }

    std::string qcodeFor(std::string_view name)
    {
        const auto lowered = lower(name);
        for (const auto& entry : kSpecialKeys)
        {
            if (entry.name == lowered)
            {
                return std::string {entry.qcode};
            }
        }
        return std::string {name};
    }

    void appendTap(std::vector<mpfshost::InputEvent>& events, const std::string& qcode)
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
        std::cerr << "usage: " << argv[0] << " <socket> <vm> [key ...]\n"
                  << "  keys: page-up, page-down, home, end, insert, delete, up, down, left, right,\n"
                  << "        escape, tab, backspace, enter, space, f1..f12, caps-lock, num-lock, or any\n"
                  << "        QEMU qcode such as pgup or kp_enter (default: page-down, page-up, home, end)\n";
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

    std::vector<std::string> keys;
    for (int index = 3; index < argc; ++index)
    {
        keys.push_back(qcodeFor(argv[index]));
    }
    if (keys.empty())
    {
        keys = {"pgdn", "pgup", "home", "end"};
    }

    std::vector<mpfshost::InputEvent> events;
    for (const auto& key : keys)
    {
        std::cout << "press " << key << '\n';
        appendTap(events, key);
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
