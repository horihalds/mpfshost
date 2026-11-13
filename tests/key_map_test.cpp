#include <catch2/catch.hpp>

#include "core/util/key_map.hpp"

#include <string>
#include <vector>

TEST_CASE("key names resolve to QEMU qcodes", "[util][key-map][input]")
{
    using mpfshost::inputKeyCode;

    REQUIRE(inputKeyCode("a").has_value());
    REQUIRE(*inputKeyCode("a") == "a");
    REQUIRE(*inputKeyCode("Enter") == "ret");
    REQUIRE(*inputKeyCode("escape") == "esc");
    REQUIRE(*inputKeyCode("space") == "spc");
    REQUIRE(*inputKeyCode("Ctrl") == "ctrl");
    REQUIRE(*inputKeyCode("f12") == "f12");
    REQUIRE(*inputKeyCode("arrowleft") == "left");
    REQUIRE_FALSE(inputKeyCode("").has_value());
    REQUIRE_FALSE(inputKeyCode("not-a-key").has_value());
    REQUIRE_FALSE(inputKeyCode("a b").has_value());
}

TEST_CASE("text expands to key events with shift for shifted characters", "[util][key-map][input]")
{
    std::vector<mpfshost::InputEvent> events;
    std::string                       error;

    REQUIRE(mpfshost::appendTextInput("aA!", events, error));
    REQUIRE(error.empty());

    REQUIRE(events.size() == 10);
    REQUIRE(events[0].kind == mpfshost::InputEventKind::Key);
    REQUIRE(events[0].key == "a");
    REQUIRE(events[0].down);
    REQUIRE_FALSE(events[1].down);

    REQUIRE(events[2].key == "shift");
    REQUIRE(events[2].down);
    REQUIRE(events[3].key == "a");
    REQUIRE(events[3].down);
    REQUIRE_FALSE(events[4].down);
    REQUIRE(events[5].key == "shift");
    REQUIRE_FALSE(events[5].down);

    REQUIRE(events[6].key == "shift");
    REQUIRE(events[6].down);
    REQUIRE(events[7].key == "1");
    REQUIRE(events[7].down);
    REQUIRE_FALSE(events[8].down);
    REQUIRE(events[9].key == "shift");
    REQUIRE_FALSE(events[9].down);
}

TEST_CASE("text maps control characters and rejects non-ASCII", "[util][key-map][input]")
{
    std::vector<mpfshost::InputEvent> events;
    std::string                       error;

    REQUIRE(mpfshost::appendTextInput("x\ny", events, error));
    REQUIRE(events.size() == 6);
    REQUIRE(events[2].key == "ret");
    REQUIRE(events[4].key == "y");

    events.clear();
    REQUIRE_FALSE(mpfshost::appendTextInput("caf\xc3\xa9", events, error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("mouse button names round trip", "[util][key-map][input]")
{
    for (const auto button :
         {mpfshost::InputMouseButton::Left, mpfshost::InputMouseButton::Middle, mpfshost::InputMouseButton::Right})
    {
        const auto name = mpfshost::inputMouseButtonName(button);
        const auto back = mpfshost::inputMouseButton(name);
        REQUIRE(back.has_value());
        REQUIRE(*back == button);
    }
    REQUIRE_FALSE(mpfshost::inputMouseButton("side").has_value());
}
