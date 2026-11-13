#include "core/util/key_map.hpp"

#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace mpfshost
{
    namespace
    {
        struct KeyName
        {
            std::string_view name;
            std::string_view code;
        };

        // QEMU's QKeyCode names plus the aliases mpfshost accepts. The names are compared case-insensitively.
        constexpr KeyName kKeyNames[] = {
            {        "shift",         "shift"},
            {      "shift_r",       "shift_r"},
            {       "lshift",         "shift"},
            {       "rshift",       "shift_r"},
            {          "alt",           "alt"},
            {        "alt_r",         "alt_r"},
            {         "lalt",           "alt"},
            {         "ralt",         "alt_r"},
            {        "altgr",         "alt_r"},
            {         "ctrl",          "ctrl"},
            {       "ctrl_r",        "ctrl_r"},
            {      "control",          "ctrl"},
            {        "lctrl",          "ctrl"},
            {        "rctrl",        "ctrl_r"},
            {       "meta_l",        "meta_l"},
            {       "meta_r",        "meta_r"},
            {          "win",        "meta_l"},
            {        "super",        "meta_l"},
            {      "windows",        "meta_l"},
            {         "menu",          "menu"},
            {          "esc",           "esc"},
            {       "escape",           "esc"},
            {            "1",             "1"},
            {            "2",             "2"},
            {            "3",             "3"},
            {            "4",             "4"},
            {            "5",             "5"},
            {            "6",             "6"},
            {            "7",             "7"},
            {            "8",             "8"},
            {            "9",             "9"},
            {            "0",             "0"},
            {        "minus",         "minus"},
            {        "equal",         "equal"},
            {    "backspace",     "backspace"},
            {         "back",     "backspace"},
            {          "tab",           "tab"},
            {            "q",             "q"},
            {            "w",             "w"},
            {            "e",             "e"},
            {            "r",             "r"},
            {            "t",             "t"},
            {            "y",             "y"},
            {            "u",             "u"},
            {            "i",             "i"},
            {            "o",             "o"},
            {            "p",             "p"},
            { "bracket_left",  "bracket_left"},
            {"bracket_right", "bracket_right"},
            {          "ret",           "ret"},
            {        "enter",           "ret"},
            {       "return",           "ret"},
            {            "a",             "a"},
            {            "s",             "s"},
            {            "d",             "d"},
            {            "f",             "f"},
            {            "g",             "g"},
            {            "h",             "h"},
            {            "j",             "j"},
            {            "k",             "k"},
            {            "l",             "l"},
            {    "semicolon",     "semicolon"},
            {   "apostrophe",    "apostrophe"},
            { "grave_accent",  "grave_accent"},
            {    "backslash",     "backslash"},
            {            "z",             "z"},
            {            "x",             "x"},
            {            "c",             "c"},
            {            "v",             "v"},
            {            "b",             "b"},
            {            "n",             "n"},
            {            "m",             "m"},
            {        "comma",         "comma"},
            {          "dot",           "dot"},
            {        "slash",         "slash"},
            {     "asterisk",      "asterisk"},
            {          "spc",           "spc"},
            {        "space",           "spc"},
            {     "spacebar",           "spc"},
            {    "caps_lock",     "caps_lock"},
            {     "capslock",     "caps_lock"},
            {           "f1",            "f1"},
            {           "f2",            "f2"},
            {           "f3",            "f3"},
            {           "f4",            "f4"},
            {           "f5",            "f5"},
            {           "f6",            "f6"},
            {           "f7",            "f7"},
            {           "f8",            "f8"},
            {           "f9",            "f9"},
            {          "f10",           "f10"},
            {          "f11",           "f11"},
            {          "f12",           "f12"},
            {     "num_lock",      "num_lock"},
            {      "numlock",      "num_lock"},
            {  "scroll_lock",   "scroll_lock"},
            {   "scrolllock",   "scroll_lock"},
            {    "kp_divide",     "kp_divide"},
            {  "kp_multiply",   "kp_multiply"},
            {  "kp_subtract",   "kp_subtract"},
            {       "kp_add",        "kp_add"},
            {     "kp_enter",      "kp_enter"},
            {   "kp_decimal",    "kp_decimal"},
            {         "kp_0",          "kp_0"},
            {         "kp_1",          "kp_1"},
            {         "kp_2",          "kp_2"},
            {         "kp_3",          "kp_3"},
            {         "kp_4",          "kp_4"},
            {         "kp_5",          "kp_5"},
            {         "kp_6",          "kp_6"},
            {         "kp_7",          "kp_7"},
            {         "kp_8",          "kp_8"},
            {         "kp_9",          "kp_9"},
            {        "sysrq",         "sysrq"},
            {        "print",         "print"},
            {  "printscreen",         "print"},
            {        "prtsc",         "print"},
            {         "home",          "home"},
            {         "pgup",          "pgup"},
            {       "pageup",          "pgup"},
            {         "pgdn",          "pgdn"},
            {     "pagedown",          "pgdn"},
            {          "end",           "end"},
            {         "left",          "left"},
            {    "arrowleft",          "left"},
            {           "up",            "up"},
            {      "arrowup",            "up"},
            {         "down",          "down"},
            {    "arrowdown",          "down"},
            {        "right",         "right"},
            {   "arrowright",         "right"},
            {       "insert",        "insert"},
            {          "ins",        "insert"},
            {       "delete",        "delete"},
            {          "del",        "delete"},
            {         "stop",          "stop"},
            {        "again",         "again"},
            {        "props",         "props"},
            {         "undo",          "undo"},
            {        "front",         "front"},
            {         "copy",          "copy"},
            {         "open",          "open"},
            {        "paste",         "paste"},
            {         "find",          "find"},
            {          "cut",           "cut"},
            {           "lf",            "lf"},
            {         "help",          "help"},
            {      "compose",       "compose"},
        };

        struct ShiftedKey
        {
            char             character;
            std::string_view code;
        };

        constexpr ShiftedKey kShiftedKeys[] = {
            {'!',             "1"},
            {'@',             "2"},
            {'#',             "3"},
            {'$',             "4"},
            {'%',             "5"},
            {'^',             "6"},
            {'&',             "7"},
            {'*',             "8"},
            {'(',             "9"},
            {')',             "0"},
            {'_',         "minus"},
            {'+',         "equal"},
            {'{',  "bracket_left"},
            {'}', "bracket_right"},
            {'|',     "backslash"},
            {':',     "semicolon"},
            {'"',    "apostrophe"},
            {'~',  "grave_accent"},
            {'<',         "comma"},
            {'>',           "dot"},
            {'?',         "slash"},
        };

        void appendKey(std::vector<InputEvent>& events, std::string_view code, bool down)
        {
            InputEvent event;
            event.kind = InputEventKind::Key;
            event.key  = code;
            event.down = down;
            events.push_back(std::move(event));
        }

        bool appendTap(std::vector<InputEvent>& events, std::string_view code, std::string& error)
        {
            if (events.size() + 2 > kMaxInputEvents)
            {
                error = "input text expands to too many key events";
                return false;
            }
            appendKey(events, code, true);
            appendKey(events, code, false);
            return true;
        }

        std::optional<std::string> plainKeyCode(char character)
        {
            if (character >= 'a' && character <= 'z')
            {
                return std::string {character};
            }
            if (character >= '0' && character <= '9')
            {
                return std::string {character};
            }

            switch (character)
            {
            case ' ':
                return std::string {"spc"};
            case '-':
                return std::string {"minus"};
            case '=':
                return std::string {"equal"};
            case '[':
                return std::string {"bracket_left"};
            case ']':
                return std::string {"bracket_right"};
            case '\\':
                return std::string {"backslash"};
            case ';':
                return std::string {"semicolon"};
            case '\'':
                return std::string {"apostrophe"};
            case '`':
                return std::string {"grave_accent"};
            case ',':
                return std::string {"comma"};
            case '.':
                return std::string {"dot"};
            case '/':
                return std::string {"slash"};
            default:
                return std::nullopt;
            }
        }

        std::optional<std::string_view> shiftedKeyCode(char character)
        {
            for (const auto& shifted : kShiftedKeys)
            {
                if (shifted.character == character)
                {
                    return shifted.code;
                }
            }
            return std::nullopt;
        }
    } // namespace

    std::optional<std::string> inputKeyCode(std::string_view name)
    {
        if (name.empty() || name.size() > kMaxInputKeyNameSize)
        {
            return std::nullopt;
        }

        std::string lowered;
        lowered.reserve(name.size());
        for (const char character : name)
        {
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        }

        for (const auto& entry : kKeyNames)
        {
            if (entry.name == lowered)
            {
                return std::string {entry.code};
            }
        }
        return std::nullopt;
    }

    bool appendTextInput(std::string_view text, std::vector<InputEvent>& events, std::string& error)
    {
        for (const char raw : text)
        {
            const auto character = static_cast<unsigned char>(raw);
            if (character == '\r')
            {
                continue;
            }
            if (character == '\n')
            {
                if (!appendTap(events, "ret", error))
                {
                    return false;
                }
                continue;
            }
            if (character == '\t')
            {
                if (!appendTap(events, "tab", error))
                {
                    return false;
                }
                continue;
            }
            if (character < 0x20 || character > 0x7E)
            {
                error = "input text must be printable ASCII";
                return false;
            }

            if (character >= 'A' && character <= 'Z')
            {
                const std::string code {static_cast<char>(std::tolower(character))};
                if (events.size() + 4 > kMaxInputEvents)
                {
                    error = "input text expands to too many key events";
                    return false;
                }
                appendKey(events, "shift", true);
                appendKey(events, code, true);
                appendKey(events, code, false);
                appendKey(events, "shift", false);
                continue;
            }

            if (const auto shifted = shiftedKeyCode(static_cast<char>(character)))
            {
                if (events.size() + 4 > kMaxInputEvents)
                {
                    error = "input text expands to too many key events";
                    return false;
                }
                appendKey(events, "shift", true);
                appendKey(events, *shifted, true);
                appendKey(events, *shifted, false);
                appendKey(events, "shift", false);
                continue;
            }

            const auto code = plainKeyCode(static_cast<char>(character));
            if (!code)
            {
                error = "input text must be printable ASCII";
                return false;
            }
            if (!appendTap(events, *code, error))
            {
                return false;
            }
        }
        return true;
    }

    std::optional<InputMouseButton> inputMouseButton(std::string_view name)
    {
        std::string lowered;
        lowered.reserve(name.size());
        for (const char character : name)
        {
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        }

        if (lowered == "left")
        {
            return InputMouseButton::Left;
        }
        if (lowered == "middle")
        {
            return InputMouseButton::Middle;
        }
        if (lowered == "right")
        {
            return InputMouseButton::Right;
        }
        return std::nullopt;
    }

    std::string_view inputMouseButtonName(InputMouseButton button) noexcept
    {
        switch (button)
        {
        case InputMouseButton::Left:
            return "left";
        case InputMouseButton::Middle:
            return "middle";
        case InputMouseButton::Right:
            return "right";
        }
        return "left";
    }
} // namespace mpfshost
