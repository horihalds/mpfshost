#include "core/client/cli_options.hpp"

#include "core/util/hex.hpp"
#include "core/util/number.hpp"

#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>

namespace mpfshost
{
    namespace
    {
        // The heap selector is the decimal heap number or the 0x heap address that `heap` reports, so a value
        // copied from either column of that listing can be passed straight back.
        std::optional<std::uint64_t> parseHeapSelector(std::string_view text)
        {
            if (text.empty())
            {
                return std::nullopt;
            }
            if (text.starts_with("0x") || text.starts_with("0X"))
            {
                return mpfshost::parseHexU64(text);
            }
            std::uint64_t parsed {};
            const auto    result = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (result.ec != std::errc {} || result.ptr != text.data() + text.size())
            {
                return std::nullopt;
            }
            return parsed;
        }

        // Config values are decimal unless written with a 0x prefix, matching the hex form MemProcFS documents
        // for a DTB.
        std::optional<std::uint64_t> parseConfigValue(std::string_view text)
        {
            if (text.starts_with("0x") || text.starts_with("0X"))
            {
                return mpfshost::parseHexU64(text);
            }
            return parseHeapSelector(text);
        }

        // Every command but list-targets and capabilities takes a mandatory target name.
        bool commandTakesTarget(const std::string& command)
        {
            return command == "bind" || command == "processes" || command == "processes-all" || command == "proc-info"
                || command == "symbol" || command == "sections" || command == "modules" || command == "maps"
                || command == "threads" || command == "read" || command == "write" || command == "scatter"
                || command == "vm-list" || command == "phys-read" || command == "phys-write"
                || command == "vm-translate" || command == "virt2phys" || command == "vquery" || command == "page"
                || command == "prefetch" || command == "search" || command == "phys-scatter" || command == "vfs-list"
                || command == "vfs-read" || command == "vfs-write" || command == "handles" || command == "eat"
                || command == "iat" || command == "unloaded" || command == "services" || command == "users"
                || command == "net" || command == "heap" || command == "heap-alloc" || command == "kdriver"
                || command == "kdevice" || command == "kobject" || command == "pte" || command == "pool"
                || command == "pool-tags" || command == "pfn" || command == "vadex" || command == "pdb-load"
                || command == "pdb-symbol" || command == "pdb-address" || command == "pdb-type-size"
                || command == "pdb-type-child" || command == "config-set" || command == "module-at"
                || command == "process-for-dtb" || command == "debug-status" || command == "debug-stop"
                || command == "debug-continue" || command == "debug-registers";
        }

        // The commands whose MemProcFS call documents dwPID = (DWORD)-1 as the target's physical memory.
        // VMMDLL_MemVirtualQuery resolves its pid through the process table and has no physical branch, so
        // vquery stays process-only.
        bool commandSupportsPhysicalMemory(const std::string& command)
        {
            return command == "page" || command == "prefetch" || command == "search";
        }

        // Every required-option check lives here so runClient receives options it can use as they are. The
        // messages are the ones the client used to report, byte for byte.
        bool validateClientOptions(const ClientOptions& options, std::string& error)
        {
            const std::string& command = options.command;
            if (command != "scatter" && (options.repeat != 1 || options.clear))
            {
                error = "--repeat and --clear are only valid for --client scatter";
                return false;
            }
            if (command != "debug-registers" && options.vcpuSet)
            {
                error = "--vcpu is only valid for --client debug-registers";
                return false;
            }
            if (command == "debug-registers" && options.vcpuSet && options.vcpu >= kMaxDebugVcpuRecords)
            {
                error = "--vcpu must be below " + std::to_string(kMaxDebugVcpuRecords);
                return false;
            }
            if (options.pid == kPhysicalPid && !commandSupportsPhysicalMemory(command))
            {
                error = "--pid -1 (physical memory) is only valid for page, prefetch and search";
                return false;
            }
            if (command == "proc-info")
            {
                if (options.pid == 0 && options.nameFilter.empty())
                {
                    error = "--client proc-info needs --pid or --name";
                    return false;
                }
            }
            else if (command == "symbol" || command == "sections")
            {
                if (options.pid == 0 || options.moduleName.empty())
                {
                    error = "--client " + command + " needs --pid and --module";
                    return false;
                }
            }
            else if (command == "module-at")
            {
                if (options.pid == 0 || !options.addressSet)
                {
                    error = "--client module-at needs --pid and --address";
                    return false;
                }
            }
            else if (command == "process-for-dtb")
            {
                if (!options.dtbSet || options.dtb == 0)
                {
                    error = "--client process-for-dtb needs a nonzero --dtb";
                    return false;
                }
            }
            else if (command == "pdb-load")
            {
                if (options.pid == 0 || !options.addressSet)
                {
                    error = "--client pdb-load needs --pid and --address";
                    return false;
                }
            }
            else if (command == "pdb-symbol")
            {
                if (options.moduleName.empty() || !options.addressSet)
                {
                    error = "--client pdb-symbol needs --module and --address";
                    return false;
                }
            }
            else if (command == "pdb-address")
            {
                if (options.moduleName.empty() || options.symbolName.empty())
                {
                    error = "--client pdb-address needs --module and --symbol";
                    return false;
                }
            }
            else if (command == "pdb-type-size")
            {
                if (options.moduleName.empty() || options.typeName.empty())
                {
                    error = "--client pdb-type-size needs --module and --type";
                    return false;
                }
            }
            else if (command == "pdb-type-child")
            {
                if (options.moduleName.empty() || options.typeName.empty() || options.childName.empty())
                {
                    error = "--client pdb-type-child needs --module, --type and --child";
                    return false;
                }
            }
            else if (command == "vadex")
            {
                if (options.pid == 0)
                {
                    error = "--client vadex needs --pid";
                    return false;
                }
            }
            else if (command == "modules" || command == "maps" || command == "threads" || command == "handles"
                     || command == "unloaded" || command == "heap")
            {
                if (options.pid == 0)
                {
                    error = "--client " + command + " needs --pid";
                    return false;
                }
            }
            else if (command == "eat" || command == "iat")
            {
                if (options.pid == 0)
                {
                    error = "--client " + command + " needs --pid";
                    return false;
                }
                if (options.moduleName.empty())
                {
                    error = "--client " + command + " needs --module";
                    return false;
                }
            }
            else if (command == "heap-alloc")
            {
                if (options.pid == 0 || !options.heapSet)
                {
                    error = "--client heap-alloc needs --pid and --heap";
                    return false;
                }
            }
            else if (command == "read")
            {
                if (options.pid == 0 || !options.addressSet || !options.sizeSet)
                {
                    error = "--client read needs --pid, --address and --size";
                    return false;
                }
            }
            else if (command == "write")
            {
                if (options.pid == 0 || !options.addressSet || !options.dataSet)
                {
                    error = "--client write needs --pid, --address and --data";
                    return false;
                }
            }
            else if (command == "scatter" || command == "page" || command == "prefetch")
            {
                if (options.pid == 0 || options.ranges.empty())
                {
                    error = "--client " + command + " needs --pid and at least one --range";
                    return false;
                }
            }
            else if (command == "phys-scatter")
            {
                if (options.ranges.empty())
                {
                    error = "--client phys-scatter needs at least one --range";
                    return false;
                }
            }
            else if (command == "phys-read")
            {
                if (!options.addressSet || !options.sizeSet)
                {
                    error = "--client phys-read needs --address and --size";
                    return false;
                }
            }
            else if (command == "phys-write")
            {
                if (!options.addressSet || !options.dataSet)
                {
                    error = "--client phys-write needs --address and --data";
                    return false;
                }
            }
            else if (command == "vfs-write")
            {
                if (!options.pathSet || !options.dataSet)
                {
                    error = "--client vfs-write needs --path and --data";
                    return false;
                }
            }
            else if (command == "vm-translate")
            {
                if (!options.addressSet)
                {
                    error = "--client vm-translate needs --address";
                    return false;
                }
            }
            else if (command == "virt2phys" || command == "vquery")
            {
                if (options.pid == 0 || !options.addressSet)
                {
                    error = "--client " + command + " needs --pid and --address";
                    return false;
                }
            }
            else if (command == "search")
            {
                if (options.pid == 0 || options.pattern.empty())
                {
                    error = "--client search needs --pid and --pattern or --hex-pattern";
                    return false;
                }
            }
            else if (command == "config-set")
            {
                if (!options.configOptionSet)
                {
                    error = "--client config-set needs --option";
                    return false;
                }
                if (configOptionUsesPid(options.configOption) && options.pid == 0)
                {
                    error = "--client config-set needs --pid for this --option";
                    return false;
                }
                if (!configOptionUsesPid(options.configOption) && options.pid != 0)
                {
                    error = "--client config-set does not take --pid for this --option";
                    return false;
                }
                if (configOptionRequiresValue(options.configOption) && !options.valueSet)
                {
                    error = "--client config-set needs --value for this --option";
                    return false;
                }
            }
            return true;
        }
    } // namespace

    std::optional<ClientOptions> parseClientOptions(std::span<const std::string_view> args, std::string& error)
    {
        ClientOptions options;
        bool          clientSeen {};

        for (std::size_t index = 0; index < args.size(); ++index)
        {
            const std::string_view argument {args[index]};
            if (argument == "--option")
            {
                if (index + 1 >= args.size())
                {
                    error = "--option needs a config option name";
                    return std::nullopt;
                }
                const auto parsed = parseConfigOption(args[++index]);
                if (!parsed)
                {
                    error = "unknown config option '" + std::string {args[index]} + "'";
                    return std::nullopt;
                }
                options.configOption    = *parsed;
                options.configOptionSet = true;
                continue;
            }
            if (argument == "--value")
            {
                if (index + 1 >= args.size())
                {
                    error = "--value needs a decimal or 0x-hex integer";
                    return std::nullopt;
                }
                const auto parsed = parseConfigValue(args[++index]);
                if (!parsed)
                {
                    error = "--value needs a decimal or 0x-hex integer";
                    return std::nullopt;
                }
                options.value    = *parsed;
                options.valueSet = true;
                continue;
            }
            if (argument == "--pid")
            {
                if (index + 1 >= args.size())
                {
                    error = "--pid needs a value";
                    return std::nullopt;
                }
                const std::string_view value {args[++index]};
                if (value == "-1")
                {
                    options.pid = mpfshost::kPhysicalPid;
                    continue;
                }
                const auto parsed = mpfshost::parseDecimalU32(value);
                if (!parsed)
                {
                    error = "--pid needs a non-negative integer";
                    return std::nullopt;
                }
                options.pid = *parsed;
                continue;
            }
            if (argument == "--tid")
            {
                if (index + 1 >= args.size())
                {
                    error = "--tid needs a value";
                    return std::nullopt;
                }
                const auto parsed = mpfshost::parseDecimalU32(args[++index]);
                if (!parsed)
                {
                    error = "--tid needs a non-negative integer";
                    return std::nullopt;
                }
                options.tid = *parsed;
                continue;
            }
            if (argument == "--callstack")
            {
                options.callstack = true;
                continue;
            }
            if (argument == "--vm")
            {
                if (index + 1 >= args.size())
                {
                    error = "--vm needs a partition index";
                    return std::nullopt;
                }
                const auto parsed = mpfshost::parseDecimalU32(args[++index]);
                if (!parsed)
                {
                    error = "--vm needs a non-negative integer";
                    return std::nullopt;
                }
                options.vmIndex = *parsed;
                continue;
            }
            if (argument == "--vcpu")
            {
                if (index + 1 >= args.size())
                {
                    error = "--vcpu needs a vCPU index";
                    return std::nullopt;
                }
                const auto parsed = mpfshost::parseDecimalU32(args[++index]);
                if (!parsed)
                {
                    error = "--vcpu needs a non-negative integer";
                    return std::nullopt;
                }
                options.vcpu    = *parsed;
                options.vcpuSet = true;
                continue;
            }
            if (argument == "--address")
            {
                if (index + 1 >= args.size())
                {
                    error = "--address needs a hex value";
                    return std::nullopt;
                }
                const auto parsed = mpfshost::parseHexU64(args[++index]);
                if (!parsed)
                {
                    error = "--address needs a hex value";
                    return std::nullopt;
                }
                options.address    = *parsed;
                options.addressSet = true;
                continue;
            }
            if (argument == "--dtb" || argument == "--cr3")
            {
                if (index + 1 >= args.size())
                {
                    error = "--" + std::string {argument.substr(2)} + " needs a hex value";
                    return std::nullopt;
                }
                const auto parsed = mpfshost::parseHexU64(args[++index]);
                if (!parsed)
                {
                    error = "--" + std::string {argument.substr(2)} + " needs a hex value";
                    return std::nullopt;
                }
                options.dtb    = *parsed;
                options.dtbSet = true;
                continue;
            }
            if (argument == "--size")
            {
                if (index + 1 >= args.size())
                {
                    error = "--size needs a positive integer";
                    return std::nullopt;
                }
                const auto parsed = mpfshost::parseDecimalU32(args[++index]);
                if (!parsed || *parsed == 0)
                {
                    error = "--size needs a positive integer";
                    return std::nullopt;
                }
                options.size    = *parsed;
                options.sizeSet = true;
                continue;
            }
            if (argument == "--data")
            {
                if (index + 1 >= args.size())
                {
                    error = "--data needs hex bytes";
                    return std::nullopt;
                }
                const auto parsed = mpfshost::parseHexBytes(args[++index]);
                if (!parsed || parsed->empty())
                {
                    error = "--data needs at least one hex byte";
                    return std::nullopt;
                }
                options.data    = *parsed;
                options.dataSet = true;
                continue;
            }
            if (argument == "--range")
            {
                if (index + 1 >= args.size())
                {
                    error = "--range needs <hex address>:<size>";
                    return std::nullopt;
                }
                const std::string_view value {args[++index]};
                const auto             separator = value.find(':');
                if (separator == std::string_view::npos)
                {
                    error = "--range needs <hex address>:<size>";
                    return std::nullopt;
                }
                const auto parsedAddress = mpfshost::parseHexU64(value.substr(0, separator));
                const auto parsedSize    = mpfshost::parseRangeSize(value.substr(separator + 1));
                if (!parsedAddress || !parsedSize || *parsedSize == 0)
                {
                    error = "--range needs <hex address>:<size>";
                    return std::nullopt;
                }
                options.ranges.push_back({*parsedAddress, *parsedSize});
                continue;
            }
            if (argument == "--repeat")
            {
                if (index + 1 >= args.size())
                {
                    error = "--repeat needs a positive integer";
                    return std::nullopt;
                }
                const auto parsed = mpfshost::parseDecimalU32(args[++index]);
                if (!parsed || *parsed == 0)
                {
                    error = "--repeat needs a positive integer";
                    return std::nullopt;
                }
                options.repeat = *parsed;
                continue;
            }
            if (argument == "--clear")
            {
                options.clear = true;
                continue;
            }
            if (argument == "--name")
            {
                if (index + 1 >= args.size())
                {
                    error = "--name needs a value";
                    return std::nullopt;
                }
                options.nameFilter = args[++index];
                continue;
            }
            if (argument == "--module")
            {
                if (index + 1 >= args.size())
                {
                    error = "--module needs a value";
                    return std::nullopt;
                }
                options.moduleName = args[++index];
                continue;
            }
            if (argument == "--symbol")
            {
                if (index + 1 >= args.size())
                {
                    error = "--symbol needs a value";
                    return std::nullopt;
                }
                options.symbolName = args[++index];
                continue;
            }
            if (argument == "--type")
            {
                if (index + 1 >= args.size())
                {
                    error = "--type needs a value";
                    return std::nullopt;
                }
                options.typeName = args[++index];
                continue;
            }
            if (argument == "--child")
            {
                if (index + 1 >= args.size())
                {
                    error = "--child needs a value";
                    return std::nullopt;
                }
                options.childName = args[++index];
                continue;
            }
            if (argument == "--counts")
            {
                options.moduleFlags |= mpfshost::kModuleFlagCounts;
                continue;
            }
            if (argument == "--debug")
            {
                options.moduleFlags |= mpfshost::kModuleFlagDebugInfo;
                continue;
            }
            if (argument == "--version-info")
            {
                options.moduleFlags |= mpfshost::kModuleFlagVersionInfo;
                continue;
            }
            if (argument == "--identify-modules")
            {
                options.identifyModules = true;
                continue;
            }
            if (argument == "--path")
            {
                if (index + 1 >= args.size())
                {
                    error = "--path needs a value";
                    return std::nullopt;
                }
                options.path    = args[++index];
                options.pathSet = true;
                continue;
            }
            if (argument == "--offset")
            {
                if (index + 1 >= args.size())
                {
                    error = "--offset needs a hex value";
                    return std::nullopt;
                }
                const auto parsed = mpfshost::parseHexU64(args[++index]);
                if (!parsed)
                {
                    error = "--offset needs a hex value";
                    return std::nullopt;
                }
                options.offset = *parsed;
                continue;
            }
            if (argument == "--pattern")
            {
                if (index + 1 >= args.size())
                {
                    error = "--pattern needs a value";
                    return std::nullopt;
                }
                options.pattern = args[++index];
                continue;
            }
            if (argument == "--hex-pattern")
            {
                if (index + 1 >= args.size())
                {
                    error = "--hex-pattern needs hex bytes";
                    return std::nullopt;
                }
                options.pattern    = args[++index];
                options.hexPattern = true;
                continue;
            }
            if (argument == "--min")
            {
                if (index + 1 >= args.size())
                {
                    error = "--min needs a hex address";
                    return std::nullopt;
                }
                const auto parsed = mpfshost::parseHexU64(args[++index]);
                if (!parsed)
                {
                    error = "--min needs a hex address";
                    return std::nullopt;
                }
                options.minAddress = *parsed;
                continue;
            }
            if (argument == "--max")
            {
                if (index + 1 >= args.size())
                {
                    error = "--max needs a hex address";
                    return std::nullopt;
                }
                const auto parsed = mpfshost::parseHexU64(args[++index]);
                if (!parsed)
                {
                    error = "--max needs a hex address";
                    return std::nullopt;
                }
                options.maxAddress = *parsed;
                continue;
            }
            if (argument == "--align")
            {
                if (index + 1 >= args.size())
                {
                    error = "--align needs a byte alignment";
                    return std::nullopt;
                }
                const auto parsed = mpfshost::parseDecimalU32(args[++index]);
                if (!parsed || *parsed == 0)
                {
                    error = "--align needs a positive integer";
                    return std::nullopt;
                }
                options.alignBytes = *parsed;
                continue;
            }
            if (argument == "--max-results")
            {
                if (index + 1 >= args.size())
                {
                    error = "--max-results needs a value";
                    return std::nullopt;
                }
                const auto parsed = mpfshost::parseDecimalU32(args[++index]);
                if (!parsed || *parsed == 0)
                {
                    error = "--max-results needs a positive integer";
                    return std::nullopt;
                }
                options.maxResults = *parsed;
                continue;
            }
            if (argument == "--heap")
            {
                if (index + 1 >= args.size())
                {
                    error = "--heap needs a heap number or a hex address";
                    return std::nullopt;
                }
                const auto parsed = parseHeapSelector(args[++index]);
                if (!parsed)
                {
                    error = "--heap needs a decimal heap number or a 0x heap address";
                    return std::nullopt;
                }
                options.heap     = *parsed;
                options.heapText = std::string(args[index]);
                options.heapSet  = true;
                continue;
            }
            if (argument == "--tag")
            {
                if (index + 1 >= args.size())
                {
                    error = "--tag needs a value";
                    return std::nullopt;
                }
                options.tagFilter = args[++index];
                continue;
            }
            if (argument == "--min-size")
            {
                if (index + 1 >= args.size())
                {
                    error = "--min-size needs a positive integer";
                    return std::nullopt;
                }
                const auto parsed = mpfshost::parseDecimalU32(args[++index]);
                if (!parsed)
                {
                    error = "--min-size needs a non-negative integer";
                    return std::nullopt;
                }
                options.minBytes = *parsed;
                continue;
            }
            if (argument == "--big")
            {
                options.bigPool = true;
                continue;
            }
            if (argument == "--pfn-from")
            {
                if (index + 1 >= args.size())
                {
                    error = "--pfn-from needs a value";
                    return std::nullopt;
                }
                const auto parsed = mpfshost::parseRangeSize(args[++index]);
                if (!parsed)
                {
                    error = "--pfn-from needs a decimal or 0x-hex page frame number";
                    return std::nullopt;
                }
                options.pfnStart = *parsed;
                continue;
            }
            if (argument == "--pfn-count")
            {
                if (index + 1 >= args.size())
                {
                    error = "--pfn-count needs a positive integer";
                    return std::nullopt;
                }
                const auto parsed = mpfshost::parseRangeSize(args[++index]);
                if (!parsed || *parsed == 0 || *parsed > mpfshost::kMaxPfnEntries)
                {
                    error = "--pfn-count needs a value between 1 and " + std::to_string(mpfshost::kMaxPfnEntries);
                    return std::nullopt;
                }
                options.pfnCount = *parsed;
                continue;
            }
            if (argument == "--page-from")
            {
                if (index + 1 >= args.size())
                {
                    error = "--page-from needs a page number";
                    return std::nullopt;
                }
                const auto parsed = mpfshost::parseDecimalU32(args[++index]);
                if (!parsed)
                {
                    error = "--page-from needs a page number";
                    return std::nullopt;
                }
                options.pageOffset = *parsed;
                continue;
            }
            if (argument == "--page-count")
            {
                if (index + 1 >= args.size())
                {
                    error = "--page-count needs a value between 1 and " + std::to_string(mpfshost::kMaxPageCount);
                    return std::nullopt;
                }
                const auto parsed = mpfshost::parseDecimalU32(args[++index]);
                if (!parsed || *parsed == 0 || *parsed > mpfshost::kMaxPageCount)
                {
                    error = "--page-count needs a value between 1 and " + std::to_string(mpfshost::kMaxPageCount);
                    return std::nullopt;
                }
                options.pageCount = *parsed;
                continue;
            }
            if (argument == "--client")
            {
                if (clientSeen)
                {
                    error = "--client was given more than once";
                    return std::nullopt;
                }
                clientSeen = true;

                if (index + 1 >= args.size())
                {
                    error = "--client needs a command";
                    return std::nullopt;
                }
                options.command = args[++index];

                if (commandTakesTarget(options.command))
                {
                    if (index + 1 >= args.size())
                    {
                        error = "--client " + options.command + " needs a target";
                        return std::nullopt;
                    }
                    options.target = args[++index];
                }
                else if (options.command != "list-targets" && options.command != "capabilities")
                {
                    error = "unknown client command '" + options.command + "'";
                    return std::nullopt;
                }
                continue;
            }

            // A token the CLI loop did not take as a server flag is a client argument it did not recognise.
            // The CLI loop consumes its own flags and their values first, so a value that itself looks like a
            // server flag would already have been taken by the CLI; that divergence only affects nonsense
            // command lines and is accepted rather than fixed with a duplicated arity table.
            error = "unknown argument '" + std::string {argument} + "'";
            return std::nullopt;
        }

        if (!clientSeen)
        {
            return std::nullopt;
        }
        if (!validateClientOptions(options, error))
        {
            return std::nullopt;
        }
        return options;
    }
} // namespace mpfshost
