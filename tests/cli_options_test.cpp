#include <catch2/catch.hpp>

#include "core/client/cli_options.hpp"
#include "core/util/number.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    struct Parsed
    {
        std::optional<mpfshost::ClientOptions> options;
        std::string                            error;
    };

    Parsed parse(const std::vector<std::string_view>& args)
    {
        Parsed result;
        result.options = mpfshost::parseClientOptions(args, result.error);
        return result;
    }

    const std::vector<std::string_view> kTargetCommands {
        "bind",
        "processes",
        "processes-all",
        "proc-info",
        "symbol",
        "sections",
        "modules",
        "maps",
        "threads",
        "read",
        "write",
        "scatter",
        "vm-list",
        "phys-read",
        "phys-write",
        "vm-translate",
        "virt2phys",
        "vquery",
        "page",
        "prefetch",
        "search",
        "phys-scatter",
        "vfs-list",
        "vfs-read",
        "vfs-write",
        "handles",
        "eat",
        "iat",
        "unloaded",
        "services",
        "users",
        "net",
        "heap",
        "heap-alloc",
        "kdriver",
        "kdevice",
        "kobject",
        "pte",
        "pool",
        "pool-tags",
        "pfn",
        "pdb-load",
        "vadex",
        "pdb-symbol",
        "pdb-address",
        "pdb-type-size",
        "pdb-type-child",
        "config-set",
        "module-at",
        "resolve",
        "process-for-dtb",
        "debug-status",
        "debug-stop",
        "debug-continue",
        "debug-registers",
        "debug-watch",
        "debug-break",
        "debug-step",
        "debug-detach",
    };

    std::vector<std::string_view> makeArgs(std::string_view command, bool withTarget)
    {
        if (withTarget)
        {
            return {"--client", command, "vm1"};
        }
        return {"--client", command};
    }
} // namespace

TEST_CASE("every target command needs a target", "[cli-options]")
{
    for (const auto command : kTargetCommands)
    {
        INFO("command: " << command);
        const auto withoutTarget = parse(makeArgs(command, false));
        REQUIRE_FALSE(withoutTarget.options.has_value());
        CHECK(withoutTarget.error == "--client " + std::string {command} + " needs a target");

        // With the target present the line may still fail a required-option check, but never on the target.
        const auto withTarget = parse(makeArgs(command, true));
        CHECK(withTarget.error.find("needs a target") == std::string::npos);
        if (withTarget.options)
        {
            CHECK(withTarget.options->command == command);
            CHECK(withTarget.options->target == "vm1");
        }
    }
}

TEST_CASE("list-targets needs no target", "[cli-options]")
{
    const auto parsed = parse({"--client", "list-targets"});
    REQUIRE(parsed.options.has_value());
    CHECK(parsed.options->command == "list-targets");
    CHECK(parsed.options->target.empty());
}

TEST_CASE("capabilities needs no target", "[cli-options]")
{
    const auto parsed = parse({"--client", "capabilities"});
    REQUIRE(parsed.options.has_value());
    CHECK(parsed.options->command == "capabilities");
    CHECK(parsed.options->target.empty());
}

TEST_CASE("a command line without --client is not a client invocation", "[cli-options]")
{
    const auto empty = parse({});
    REQUIRE_FALSE(empty.options.has_value());
    CHECK(empty.error.empty());

    const auto onlyFlags = parse({"--pid", "42", "--size", "16"});
    REQUIRE_FALSE(onlyFlags.options.has_value());
    CHECK(onlyFlags.error.empty());
}

TEST_CASE("client command errors are reported verbatim", "[cli-options]")
{
    const auto unknownCommand = parse({"--client", "nope"});
    REQUIRE_FALSE(unknownCommand.options.has_value());
    CHECK(unknownCommand.error == "unknown client command 'nope'");

    const auto missingCommand = parse({"--client"});
    REQUIRE_FALSE(missingCommand.options.has_value());
    CHECK(missingCommand.error == "--client needs a command");

    const auto twice = parse({"--client", "vm-list", "vm1", "--client", "vm-list", "vm2"});
    REQUIRE_FALSE(twice.options.has_value());
    CHECK(twice.error == "--client was given more than once");

    const auto unknownArgument = parse({"--client", "vm-list", "vm1", "--bogus"});
    REQUIRE_FALSE(unknownArgument.options.has_value());
    CHECK(unknownArgument.error == "unknown argument '--bogus'");
}

TEST_CASE("client value flags keep hex and decimal apart", "[cli-options]")
{
    const auto parsed =
        parse({"--client", "read", "vm1", "--pid", "42", "--address", "0x1f0", "--size", "64", "--offset", "10"});
    REQUIRE(parsed.options.has_value());
    CHECK(parsed.options->pid == 42);
    CHECK(parsed.options->address == 0x1F0);
    CHECK(parsed.options->addressSet);
    CHECK(parsed.options->size == 64);
    CHECK(parsed.options->sizeSet);
    // --offset is hex, so "10" is sixteen.
    CHECK(parsed.options->offset == 0x10);
}

TEST_CASE("a repeated --range appends in command-line order", "[cli-options]")
{
    const auto parsed =
        parse({"--client", "scatter", "vm1", "--pid", "1", "--range", "0x1000:0x100", "--range", "0x2000:4096"});
    REQUIRE(parsed.options.has_value());
    REQUIRE(parsed.options->ranges.size() == 2);
    CHECK(parsed.options->ranges[0].address == 0x1000);
    CHECK(parsed.options->ranges[0].size == 0x100);
    CHECK(parsed.options->ranges[1].address == 0x2000);
    CHECK(parsed.options->ranges[1].size == 4096);
}

TEST_CASE("scatter repeat and clear flags are parsed", "[cli-options]")
{
    const auto parsed =
        parse({"--client", "scatter", "vm1", "--pid", "1", "--range", "0x1000:16", "--repeat", "3", "--clear"});
    REQUIRE(parsed.options.has_value());
    CHECK(parsed.options->repeat == 3);
    CHECK(parsed.options->clear);
}

TEST_CASE("scatter repeat and clear are rejected for other commands", "[cli-options]")
{
    const auto repeat =
        parse({"--client", "read", "vm1", "--pid", "1", "--address", "0x1000", "--size", "16", "--repeat", "2"});
    REQUIRE_FALSE(repeat.options.has_value());
    CHECK(repeat.error == "--repeat and --clear are only valid for --client scatter");

    const auto clear =
        parse({"--client", "read", "vm1", "--pid", "1", "--address", "0x1000", "--size", "16", "--clear"});
    REQUIRE_FALSE(clear.options.has_value());
    CHECK(clear.error == "--repeat and --clear are only valid for --client scatter");
}

TEST_CASE("a malformed repeat value is rejected", "[cli-options]")
{
    for (const std::string_view value : {"0", "zz"})
    {
        INFO("value: " << value);
        const auto parsed =
            parse({"--client", "scatter", "vm1", "--pid", "1", "--range", "0x1000:16", "--repeat", value});
        REQUIRE_FALSE(parsed.options.has_value());
        CHECK(parsed.error == "--repeat needs a positive integer");
    }
}

TEST_CASE("malformed ranges are rejected", "[cli-options]")
{
    for (const std::string_view value : {"0x1000", "0x1000:0", "zz:0x10", "0x1000:zz"})
    {
        INFO("value: " << value);
        const auto parsed = parse({"--client", "scatter", "vm1", "--pid", "1", "--range", value});
        REQUIRE_FALSE(parsed.options.has_value());
        CHECK(parsed.error == "--range needs <hex address>:<size>");
    }
}

TEST_CASE("client hex payload flags are parsed as bytes", "[cli-options]")
{
    const auto data = parse({"--client", "write", "vm1", "--pid", "1", "--address", "0x10", "--data", "deadbeef"});
    REQUIRE(data.options.has_value());
    REQUIRE(data.options->data.size() == 4);
    CHECK(data.options->data[0] == std::byte {0xDE});
    CHECK(data.options->data[3] == std::byte {0xEF});
    CHECK(data.options->dataSet);

    const auto hexPattern = parse({"--client", "search", "vm1", "--pid", "1", "--hex-pattern", "de??ef"});
    REQUIRE(hexPattern.options.has_value());
    CHECK(hexPattern.options->hexPattern);
    CHECK(hexPattern.options->pattern == "de??ef");

    const auto textPattern = parse({"--client", "search", "vm1", "--pid", "1", "--pattern", "needle"});
    REQUIRE(textPattern.options.has_value());
    CHECK_FALSE(textPattern.options->hexPattern);
    CHECK(textPattern.options->pattern == "needle");
}

TEST_CASE("the last --hex-pattern wins", "[cli-options]")
{
    const auto parsed = parse(
        {"--client", "search", "vm1", "--pid", "1", "--hex-pattern", "aa", "--pattern", "x", "--hex-pattern", "bb"});
    REQUIRE(parsed.options.has_value());
    CHECK(parsed.options->hexPattern);
    CHECK(parsed.options->pattern == "bb");
}

TEST_CASE("zero is rejected for every positive-only client flag", "[cli-options]")
{
    SECTION("--size")
    {
        const auto parsed = parse({"--client", "read", "vm1", "--pid", "1", "--address", "0x10", "--size", "0"});
        REQUIRE_FALSE(parsed.options.has_value());
        CHECK(parsed.error == "--size needs a positive integer");
    }
    SECTION("--align")
    {
        const auto parsed = parse({"--client", "search", "vm1", "--pid", "1", "--pattern", "p", "--align", "0"});
        REQUIRE_FALSE(parsed.options.has_value());
        CHECK(parsed.error == "--align needs a positive integer");
    }
    SECTION("--max-results")
    {
        const auto parsed = parse({"--client", "search", "vm1", "--pid", "1", "--pattern", "p", "--max-results", "0"});
        REQUIRE_FALSE(parsed.options.has_value());
        CHECK(parsed.error == "--max-results needs a positive integer");
    }
}

TEST_CASE("malformed client flag values keep their documented message", "[cli-options]")
{
    struct Case
    {
        std::vector<std::string_view> args;
        std::string_view              message;
    };

    const std::vector<Case> cases {
        {     {"--client", "vm-list", "vm1", "--pid", "-2"},                      "--pid needs a non-negative integer"},
        { {"--client", "vm-list", "vm1", "--address", "zz"},                             "--address needs a hex value"},
        {  {"--client", "vm-list", "vm1", "--offset", "zz"},                              "--offset needs a hex value"},
        {     {"--client", "vm-list", "vm1", "--min", "zz"},                               "--min needs a hex address"},
        {     {"--client", "vm-list", "vm1", "--max", "zz"},                               "--max needs a hex address"},
        {    {"--client", "vm-list", "vm1", "--heap", "zz"}, "--heap needs a decimal heap number or a 0x heap address"},
        {    {"--client", "vm-list", "vm1", "--data", "zz"},                      "--data needs at least one hex byte"},
        {{"--client", "vm-list", "vm1", "--pfn-from", "zz"},  "--pfn-from needs a decimal or 0x-hex page frame number"},
        {     {"--client", "vm-list", "vm1", "--tid", "-1"},                      "--tid needs a non-negative integer"},
        {      {"--client", "vm-list", "vm1", "--vm", "-1"},                       "--vm needs a non-negative integer"},
        {{"--client", "vm-list", "vm1", "--min-size", "-1"},                 "--min-size needs a non-negative integer"},
    };

    for (const auto& testCase : cases)
    {
        INFO("message: " << testCase.message);
        const auto parsed = parse(testCase.args);
        REQUIRE_FALSE(parsed.options.has_value());
        CHECK(parsed.error == testCase.message);
    }
}

TEST_CASE("module-at and process-for-dtb options parse", "[cli-options]")
{
    const auto moduleAt = parse({"--client", "module-at", "vm1", "--pid", "42", "--address", "0x7ffb10001000"});
    REQUIRE(moduleAt.options.has_value());
    CHECK(moduleAt.options->pid == 42u);
    CHECK(moduleAt.options->addressSet);
    CHECK(moduleAt.options->address == 0x7ffb10001000ull);

    const auto resolve = parse({"--client", "resolve", "vm1", "--pid", "42", "--address", "0x7ffb10001000"});
    REQUIRE(resolve.options.has_value());
    CHECK(resolve.options->command == "resolve");
    CHECK(resolve.options->pid == 42u);
    CHECK(resolve.options->addressSet);
    CHECK(resolve.options->address == 0x7ffb10001000ull);

    const auto dtb = parse({"--client", "process-for-dtb", "vm1", "--dtb", "0x1aafff"});
    REQUIRE(dtb.options.has_value());
    CHECK(dtb.options->dtbSet);
    CHECK(dtb.options->dtb == 0x1aafffull);

    const auto cr3 = parse({"--client", "process-for-dtb", "vm1", "--cr3", "0x1aa000"});
    REQUIRE(cr3.options.has_value());
    CHECK(cr3.options->dtbSet);
    CHECK(cr3.options->dtb == 0x1aa000ull);

    const auto zero = parse({"--client", "process-for-dtb", "vm1", "--dtb", "0"});
    REQUIRE_FALSE(zero.options.has_value());
    CHECK(zero.error == "--client process-for-dtb needs a nonzero --dtb");

    const auto bad = parse({"--client", "process-for-dtb", "vm1", "--cr3", "zz"});
    REQUIRE_FALSE(bad.options.has_value());
    CHECK(bad.error == "--cr3 needs a hex value");
}

TEST_CASE("the physical pid selects physical memory for the memory commands", "[cli-options]")
{
    const auto page = parse({"--client", "page", "vm1", "--pid", "-1", "--range", "0x1000:0x1000"});
    REQUIRE(page.options.has_value());
    CHECK(page.options->pid == mpfshost::kPhysicalPid);

    const auto prefetch = parse({"--client", "prefetch", "vm1", "--pid", "-1", "--range", "0x1000:0x1000"});
    REQUIRE(prefetch.options.has_value());
    CHECK(prefetch.options->pid == mpfshost::kPhysicalPid);

    const auto search = parse({"--client", "search", "vm1", "--pid", "-1", "--pattern", "needle"});
    REQUIRE(search.options.has_value());
    CHECK(search.options->pid == mpfshost::kPhysicalPid);
}

TEST_CASE("the physical pid is rejected for a command that needs a process", "[cli-options]")
{
    const auto modules = parse({"--client", "modules", "vm1", "--pid", "-1"});
    REQUIRE_FALSE(modules.options.has_value());
    CHECK(modules.error == "--pid -1 (physical memory) is only valid for page, prefetch and search");

    // VMMDLL_MemVirtualQuery has no physical-memory branch, so vquery stays process-only.
    const auto vquery = parse({"--client", "vquery", "vm1", "--pid", "-1", "--address", "0x1000"});
    REQUIRE_FALSE(vquery.options.has_value());
    CHECK(vquery.error == "--pid -1 (physical memory) is only valid for page, prefetch and search");
}

TEST_CASE("config-set parses its option, value and process pid", "[cli-options]")
{
    const auto ticks = parse({"--client", "config-set", "vm1", "--option", "readcache-ticks", "--value", "0x10"});
    REQUIRE(ticks.options.has_value());
    CHECK(ticks.options->configOption == mpfshost::ConfigOption::ReadCacheTicks);
    CHECK(ticks.options->value == 0x10u);
    CHECK(ticks.options->pid == 0u);

    const auto refresh = parse({"--client", "config-set", "vm1", "--option", "refresh-all"});
    REQUIRE(refresh.options.has_value());
    CHECK(refresh.options->configOption == mpfshost::ConfigOption::RefreshAll);
    CHECK_FALSE(refresh.options->valueSet);

    const auto dtb =
        parse({"--client", "config-set", "vm1", "--option", "process-dtb", "--pid", "4", "--value", "0x1000"});
    REQUIRE(dtb.options.has_value());
    CHECK(dtb.options->configOption == mpfshost::ConfigOption::ProcessDtb);
    CHECK(dtb.options->pid == 4u);
    CHECK(dtb.options->value == 0x1000u);

    const auto unknown = parse({"--client", "config-set", "vm1", "--option", "no-such-option"});
    REQUIRE_FALSE(unknown.options.has_value());
    CHECK(unknown.error == "unknown config option 'no-such-option'");

    const auto missingPid = parse({"--client", "config-set", "vm1", "--option", "refresh-specific-process"});
    REQUIRE_FALSE(missingPid.options.has_value());
    CHECK(missingPid.error == "--client config-set needs --pid for this --option");

    const auto extraPid = parse({"--client", "config-set", "vm1", "--option", "refresh-all", "--pid", "4"});
    REQUIRE_FALSE(extraPid.options.has_value());
    CHECK(extraPid.error == "--client config-set does not take --pid for this --option");

    const auto missingValue = parse({"--client", "config-set", "vm1", "--option", "tick-period"});
    REQUIRE_FALSE(missingValue.options.has_value());
    CHECK(missingValue.error == "--client config-set needs --value for this --option");
}

TEST_CASE("the pfn-count flag stays inside its documented bounds", "[cli-options]")
{
    const std::string aboveCap = std::to_string(mpfshost::kMaxPfnEntries + 1);
    for (const std::string& value : {std::string {"0"}, aboveCap})
    {
        INFO("value: " << value);
        const auto parsed = parse({"--client", "pfn", "vm1", "--pfn-count", value});
        REQUIRE_FALSE(parsed.options.has_value());
        CHECK(parsed.error == "--pfn-count needs a value between 1 and " + std::to_string(mpfshost::kMaxPfnEntries));
    }

    const auto accepted = parse({"--client", "pfn", "vm1", "--pfn-count", "16", "--pfn-from", "0x20"});
    REQUIRE(accepted.options.has_value());
    CHECK(accepted.options->pfnCount == 16);
    CHECK(accepted.options->pfnStart == 0x20);
}

TEST_CASE("the vadex page window flags parse and enforce the page cap", "[cli-options]")
{
    const auto noPid = parse({"--client", "vadex", "vm1"});
    REQUIRE_FALSE(noPid.options.has_value());
    CHECK(noPid.error == "--client vadex needs --pid");

    const std::string aboveCap = std::to_string(mpfshost::kMaxPageCount + 1);
    for (const std::string& value : {std::string {"0"}, aboveCap})
    {
        INFO("value: " << value);
        const auto parsed = parse({"--client", "vadex", "vm1", "--pid", "1", "--page-count", value});
        REQUIRE_FALSE(parsed.options.has_value());
        CHECK(parsed.error == "--page-count needs a value between 1 and " + std::to_string(mpfshost::kMaxPageCount));
    }

    const auto accepted = parse({"--client", "vadex", "vm1", "--pid", "1", "--page-from", "8", "--page-count", "32"});
    REQUIRE(accepted.options.has_value());
    CHECK(accepted.options->pageOffset == 8);
    CHECK(accepted.options->pageCount == 32);
}

TEST_CASE("the heap selector takes a heap number or a heap address", "[cli-options]")
{
    const auto decimal = parse({"--client", "heap-alloc", "vm1", "--pid", "1", "--heap", "7"});
    REQUIRE(decimal.options.has_value());
    CHECK(decimal.options->heap == 7);
    CHECK(decimal.options->heapText == "7");
    CHECK(decimal.options->heapSet);

    const auto address = parse({"--client", "heap-alloc", "vm1", "--pid", "1", "--heap", "0xdead0000"});
    REQUIRE(address.options.has_value());
    CHECK(address.options->heap == 0xDEAD0000);
    CHECK(address.options->heapText == "0xdead0000");
    CHECK(address.options->heapSet);
}

TEST_CASE("module flags accumulate", "[cli-options]")
{
    const auto parsed = parse({"--client", "modules", "vm1", "--pid", "1", "--counts", "--debug", "--version-info"});
    REQUIRE(parsed.options.has_value());
    CHECK((parsed.options->moduleFlags & mpfshost::kModuleFlagCounts) != 0);
    CHECK((parsed.options->moduleFlags & mpfshost::kModuleFlagDebugInfo) != 0);
    CHECK((parsed.options->moduleFlags & mpfshost::kModuleFlagVersionInfo) != 0);
}

TEST_CASE("a missing required option names the command that needs it", "[cli-options]")
{
    struct Case
    {
        std::vector<std::string_view> args;
        std::string_view              message;
    };

    const std::vector<Case> cases {
        {               {"--client", "proc-info", "vm1"},                   "--client proc-info needs --pid or --name"},
        {                  {"--client", "symbol", "vm1"},                   "--client symbol needs --pid and --module"},
        {                {"--client", "sections", "vm1"},                 "--client sections needs --pid and --module"},
        {               {"--client", "module-at", "vm1"},               "--client module-at needs --pid and --address"},
        {                 {"--client", "resolve", "vm1"},                 "--client resolve needs --pid and --address"},
        {         {"--client", "process-for-dtb", "vm1"},             "--client process-for-dtb needs a nonzero --dtb"},
        {                 {"--client", "modules", "vm1"},                               "--client modules needs --pid"},
        {                    {"--client", "maps", "vm1"},                                  "--client maps needs --pid"},
        {                 {"--client", "threads", "vm1"},                               "--client threads needs --pid"},
        {                 {"--client", "handles", "vm1"},                               "--client handles needs --pid"},
        {                {"--client", "unloaded", "vm1"},                              "--client unloaded needs --pid"},
        {                    {"--client", "heap", "vm1"},                                  "--client heap needs --pid"},
        {                     {"--client", "eat", "vm1"},                                   "--client eat needs --pid"},
        {                     {"--client", "iat", "vm1"},                                   "--client iat needs --pid"},
        {       {"--client", "eat", "vm1", "--pid", "1"},                                "--client eat needs --module"},
        {       {"--client", "iat", "vm1", "--pid", "1"},                                "--client iat needs --module"},
        {              {"--client", "heap-alloc", "vm1"},                 "--client heap-alloc needs --pid and --heap"},
        {{"--client", "heap-alloc", "vm1", "--pid", "1"},                 "--client heap-alloc needs --pid and --heap"},
        {                    {"--client", "read", "vm1"},            "--client read needs --pid, --address and --size"},
        {                   {"--client", "write", "vm1"},           "--client write needs --pid, --address and --data"},
        {                 {"--client", "scatter", "vm1"},      "--client scatter needs --pid and at least one --range"},
        {                    {"--client", "page", "vm1"},         "--client page needs --pid and at least one --range"},
        {                {"--client", "prefetch", "vm1"},     "--client prefetch needs --pid and at least one --range"},
        {            {"--client", "phys-scatter", "vm1"},           "--client phys-scatter needs at least one --range"},
        {               {"--client", "phys-read", "vm1"},              "--client phys-read needs --address and --size"},
        {              {"--client", "phys-write", "vm1"},             "--client phys-write needs --address and --data"},
        {            {"--client", "vm-translate", "vm1"},                      "--client vm-translate needs --address"},
        {               {"--client", "virt2phys", "vm1"},               "--client virt2phys needs --pid and --address"},
        {                  {"--client", "vquery", "vm1"},                  "--client vquery needs --pid and --address"},
        {                  {"--client", "search", "vm1"}, "--client search needs --pid and --pattern or --hex-pattern"},
        {                {"--client", "pdb-load", "vm1"},                "--client pdb-load needs --pid and --address"},
        {              {"--client", "pdb-symbol", "vm1"},           "--client pdb-symbol needs --module and --address"},
        {             {"--client", "pdb-address", "vm1"},           "--client pdb-address needs --module and --symbol"},
        {           {"--client", "pdb-type-size", "vm1"},           "--client pdb-type-size needs --module and --type"},
        {          {"--client", "pdb-type-child", "vm1"}, "--client pdb-type-child needs --module, --type and --child"},
        {              {"--client", "config-set", "vm1"},                         "--client config-set needs --option"},
    };

    for (const auto& testCase : cases)
    {
        INFO("message: " << testCase.message);
        const auto parsed = parse(testCase.args);
        REQUIRE_FALSE(parsed.options.has_value());
        CHECK(parsed.error == testCase.message);
    }
}

TEST_CASE("a command line with every required option parses", "[cli-options]")
{
    const std::vector<std::vector<std::string_view>> lines {
        {"--client", "proc-info", "vm1", "--name", "lsass"},
        {"--client", "symbol", "vm1", "--pid", "1", "--module", "ntoskrnl.exe"},
        {"--client", "sections", "vm1", "--pid", "1", "--module", "kernel32.dll"},
        {"--client", "module-at", "vm1", "--pid", "1", "--address", "0x7ffb10001000"},
        {"--client", "process-for-dtb", "vm1", "--dtb", "0x1aa000"},
        {"--client", "process-for-dtb", "vm1", "--cr3", "0x1aafff"},
        {"--client", "modules", "vm1", "--pid", "1"},
        {"--client", "maps", "vm1", "--pid", "1"},
        {"--client", "threads", "vm1", "--pid", "1"},
        {"--client", "handles", "vm1", "--pid", "1"},
        {"--client", "unloaded", "vm1", "--pid", "1"},
        {"--client", "heap", "vm1", "--pid", "1"},
        {"--client", "eat", "vm1", "--pid", "1", "--module", "kernel32.dll"},
        {"--client", "iat", "vm1", "--pid", "1", "--module", "kernel32.dll"},
        {"--client", "heap-alloc", "vm1", "--pid", "1", "--heap", "3"},
        {"--client", "read", "vm1", "--pid", "1", "--address", "0x1000", "--size", "16"},
        {"--client", "write", "vm1", "--pid", "1", "--address", "0x1000", "--data", "aa"},
        {"--client", "scatter", "vm1", "--pid", "1", "--range", "0x1000:16"},
        {"--client", "page", "vm1", "--pid", "1", "--range", "0x1000:16"},
        {"--client", "prefetch", "vm1", "--pid", "1", "--range", "0x1000:16"},
        {"--client", "phys-scatter", "vm1", "--range", "0x1000:16"},
        {"--client", "phys-read", "vm1", "--address", "0x1000", "--size", "16"},
        {"--client", "phys-write", "vm1", "--address", "0x1000", "--data", "aa"},
        {"--client", "vm-translate", "vm1", "--address", "0x1000"},
        {"--client", "virt2phys", "vm1", "--pid", "1", "--address", "0x1000"},
        {"--client", "vquery", "vm1", "--pid", "1", "--address", "0x1000"},
        {"--client", "search", "vm1", "--pid", "1", "--pattern", "needle"},
        {"--client", "pdb-load", "vm1", "--pid", "1", "--address", "0xfffff80000000000"},
        {"--client", "pdb-symbol", "vm1", "--module", "nt", "--address", "0xfffff80000123456"},
        {"--client", "pdb-address", "vm1", "--module", "nt", "--symbol", "PsGetCurrentProcess"},
        {"--client", "pdb-type-size", "vm1", "--module", "nt", "--type", "_EPROCESS"},
        {"--client", "pdb-type-child", "vm1", "--module", "nt", "--type", "_EPROCESS", "--child", "UniqueProcessId"},
        {"--client", "config-set", "vm1", "--option", "readcache-ticks", "--value", "300"},
        {"--client", "config-set", "vm1", "--option", "refresh-all"},
        {"--client", "config-set", "vm1", "--option", "process-dtb", "--pid", "4", "--value", "0x1aa000"},
    };

    for (const auto& line : lines)
    {
        INFO("command: " << line[1]);
        const auto parsed = parse(line);
        REQUIRE(parsed.options.has_value());
        CHECK(parsed.error.empty());
    }
}

TEST_CASE("debug watch options are parsed and validated", "[cli-options]")
{
    const auto parsed = parse({"--client",
                               "debug-watch",
                               "vm1",
                               "--pid",
                               "42",
                               "--address",
                               "0x5000",
                               "--width",
                               "2",
                               "--access",
                               "write",
                               "--max-hits",
                               "3",
                               "--timeout",
                               "5"});
    REQUIRE(parsed.options.has_value());
    CHECK(parsed.options->watchWidth == 2);
    CHECK(parsed.options->watchAccess == mpfshost::DebugWatchAccess::Write);
    CHECK(parsed.options->maxHits == 3);
    CHECK(parsed.options->watchTimeoutSeconds == 5);

    const auto badWidth =
        parse({"--client", "debug-watch", "vm1", "--pid", "42", "--address", "0x5000", "--width", "3"});
    REQUIRE_FALSE(badWidth.options.has_value());
    CHECK(badWidth.error.find("--width") != std::string::npos);

    const auto badAccess =
        parse({"--client", "debug-watch", "vm1", "--pid", "42", "--address", "0x5000", "--access", "read"});
    REQUIRE_FALSE(badAccess.options.has_value());
    CHECK(badAccess.error.find("--access") != std::string::npos);

    const auto misplaced =
        parse({"--client", "read", "vm1", "--pid", "42", "--address", "0x5000", "--size", "16", "--width", "2"});
    REQUIRE_FALSE(misplaced.options.has_value());
    CHECK(misplaced.error.find("only valid") != std::string::npos);
}

TEST_CASE("debug breakpoint and step options are parsed and validated", "[cli-options]")
{
    const auto parsed = parse({"--client",
                               "debug-break",
                               "vm1",
                               "--pid",
                               "42",
                               "--address",
                               "0x5000",
                               "--kind",
                               "hardware",
                               "--timeout",
                               "5"});
    REQUIRE(parsed.options.has_value());
    CHECK(parsed.options->breakpointKind == mpfshost::DebugBreakpointKind::Hardware);
    CHECK(parsed.options->watchTimeoutSeconds == 5);

    const auto badKind =
        parse({"--client", "debug-break", "vm1", "--pid", "42", "--address", "0x5000", "--kind", "firmware"});
    REQUIRE_FALSE(badKind.options.has_value());
    CHECK(badKind.error.find("--kind") != std::string::npos);

    const auto misplacedKind = parse({"--client", "debug-step", "vm1", "--vcpu", "1", "--kind", "software"});
    REQUIRE_FALSE(misplacedKind.options.has_value());
    CHECK(misplacedKind.error.find("--kind") != std::string::npos);

    const auto step = parse({"--client", "debug-step", "vm1", "--vcpu", "3"});
    REQUIRE(step.options.has_value());
    CHECK(step.options->vcpu == 3);

    const auto stepRange = parse({"--client", "debug-step", "vm1", "--vcpu", "256"});
    REQUIRE_FALSE(stepRange.options.has_value());
    CHECK(stepRange.error.find("--vcpu") != std::string::npos);

    const auto stepThread = parse({"--client", "debug-step", "vm1", "--thread", "p01.03"});
    REQUIRE(stepThread.options.has_value());
    CHECK(stepThread.options->threadSet);
    CHECK(stepThread.options->threadId == "p01.03");

    const auto bothSelectors = parse({"--client", "debug-step", "vm1", "--vcpu", "1", "--thread", "p01.03"});
    REQUIRE_FALSE(bothSelectors.options.has_value());
    CHECK(bothSelectors.error.find("--thread") != std::string::npos);

    const auto misplacedThread = parse({"--client", "debug-registers", "vm1", "--thread", "p01.03"});
    REQUIRE_FALSE(misplacedThread.options.has_value());
    CHECK(misplacedThread.error.find("--thread") != std::string::npos);

    const auto threads = parse({"--client", "debug-threads", "vm1"});
    REQUIRE(threads.options.has_value());
    CHECK(threads.options->command == "debug-threads");

    const auto disassemble =
        parse({"--client", "disassemble", "vm1", "--pid", "42", "--address", "0x401000", "--count", "8"});
    REQUIRE(disassemble.options.has_value());
    CHECK(disassemble.options->command == "disassemble");
    CHECK(disassemble.options->count == 8);

    const auto badCount =
        parse({"--client", "disassemble", "vm1", "--pid", "42", "--address", "0x401000", "--count", "0"});
    REQUIRE_FALSE(badCount.options.has_value());
    CHECK(badCount.error.find("--count") != std::string::npos);

    const auto backtrace =
        parse({"--client", "debug-backtrace", "vm1", "--pid", "42", "--thread", "p01.02", "--max-frames", "12"});
    REQUIRE(backtrace.options.has_value());
    CHECK(backtrace.options->command == "debug-backtrace");
    CHECK(backtrace.options->threadId == "p01.02");
    CHECK(backtrace.options->maxFrames == 12);

    const auto badFrames = parse({"--client", "debug-backtrace", "vm1", "--pid", "42", "--max-frames", "0"});
    REQUIRE_FALSE(badFrames.options.has_value());
    CHECK(badFrames.error.find("--max-frames") != std::string::npos);
}

TEST_CASE("decimal u32 values are parsed strictly", "[util][number]")
{
    CHECK(mpfshost::parseDecimalU32("0") == 0u);
    CHECK(mpfshost::parseDecimalU32("42") == 42u);
    CHECK(mpfshost::parseDecimalU32("4294967295") == 4294967295u);

    CHECK_FALSE(mpfshost::parseDecimalU32("").has_value());
    CHECK_FALSE(mpfshost::parseDecimalU32("-1").has_value());
    CHECK_FALSE(mpfshost::parseDecimalU32("+1").has_value());
    CHECK_FALSE(mpfshost::parseDecimalU32("12x").has_value());
    CHECK_FALSE(mpfshost::parseDecimalU32("99999999999").has_value());
}

TEST_CASE("range sizes take decimal or 0x-hex input", "[util][number]")
{
    CHECK(mpfshost::parseRangeSize("4096") == 4096u);
    CHECK(mpfshost::parseRangeSize("0x1000") == 4096u);
    CHECK(mpfshost::parseRangeSize("0X10") == 16u);

    CHECK_FALSE(mpfshost::parseRangeSize("").has_value());
    CHECK_FALSE(mpfshost::parseRangeSize("0x").has_value());
    CHECK_FALSE(mpfshost::parseRangeSize("0x100000000").has_value());
    CHECK_FALSE(mpfshost::parseRangeSize("99999999999").has_value());
}

TEST_CASE("persistent debug commands are parsed and validated", "[cli-options]")
{
    const auto armBreak =
        parse({"--client", "debug-arm-break", "vm1", "--pid", "42", "--address", "0x1000", "--kind", "hardware"});
    REQUIRE(armBreak.options.has_value());
    CHECK(armBreak.options->command == "debug-arm-break");
    CHECK(armBreak.options->pid == 42);
    CHECK(armBreak.options->address == 0x1000);
    CHECK(armBreak.options->breakpointKind == mpfshost::DebugBreakpointKind::Hardware);

    const auto missingPid = parse({"--client", "debug-arm-watch", "vm1", "--address", "0x1000"});
    REQUIRE_FALSE(missingPid.options.has_value());
    CHECK(missingPid.error.find("--pid") != std::string::npos);

    const auto armWatch =
        parse({"--client", "debug-arm-watch", "vm1", "--pid", "42", "--address", "0x2000", "--width", "8"});
    REQUIRE(armWatch.options.has_value());
    CHECK(armWatch.options->watchWidth == 8);

    const auto remove = parse({"--client", "debug-remove-break", "vm1", "--id", "7"});
    REQUIRE(remove.options.has_value());
    CHECK(remove.options->resourceId == 7);

    const auto missingId = parse({"--client", "debug-remove-watch", "vm1"});
    REQUIRE_FALSE(missingId.options.has_value());
    CHECK(missingId.error.find("--id") != std::string::npos);

    const auto misplacedId = parse({"--client", "debug-breakpoints", "vm1", "--id", "7"});
    REQUIRE_FALSE(misplacedId.options.has_value());
    CHECK(misplacedId.error.find("--id") != std::string::npos);

    const auto wait = parse({"--client", "debug-wait", "vm1", "--max-hits", "4", "--timeout", "10"});
    REQUIRE(wait.options.has_value());
    CHECK(wait.options->maxHits == 4);
    CHECK(wait.options->watchTimeoutSeconds == 10);
}
