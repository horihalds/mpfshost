#include "core/mcp/mcp_tools.hpp"

#include "core/client/client.hpp"
#include "core/ipc/frame.hpp"
#include "core/util/hex.hpp"
#include "core/util/number.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mpfshost::mcp
{
    namespace
    {
        using Json = nlohmann::json;

        constexpr std::size_t   kDefaultListLimit     = 100;
        constexpr std::size_t   kMaxListLimit         = 1000;
        constexpr std::uint32_t kDefaultReadSize      = 256;
        constexpr std::uint32_t kMaxReadSizeMcp       = 1u << 20; // 1 MiB keeps a tool result usable by a model
        constexpr std::uint32_t kDefaultVfsReadSize   = 4096;
        constexpr std::uint32_t kDefaultSearchResults = 1024;

        struct ArgumentError : std::runtime_error
        {
            using std::runtime_error::runtime_error;
        };

        struct ParamSpec
        {
            const char* name;
            const char* type;
            const char* description;
            bool        required;
        };

        struct ToolSpec
        {
            const char*            name;
            const char*            title;
            const char*            description;
            bool                   readOnly;
            std::vector<ParamSpec> params;
        };

        std::string hexString(std::uint64_t value)
        {
            char buffer[19] {};
            std::snprintf(buffer, sizeof(buffer), "0x%016llx", static_cast<unsigned long long>(value));
            return buffer;
        }

        std::string hexSmall(std::uint64_t value)
        {
            char buffer[19] {};
            std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
            return buffer;
        }

        std::string poolTagText(std::uint32_t tag)
        {
            std::string text;
            for (int shift = 0; shift < 32; shift += 8)
            {
                const auto byte = static_cast<char>((tag >> shift) & 0xFF);
                text.push_back(byte >= 0x20 && byte < 0x7F ? byte : '.');
            }
            return text;
        }

        Json bytesJson(std::span<const std::byte> data)
        {
            static constexpr char kHexDigits[] = "0123456789abcdef";

            std::string hex;
            hex.reserve(data.size() * 2);
            std::string ascii;
            ascii.reserve(data.size());
            for (const auto byte : data)
            {
                const auto value = std::to_integer<unsigned>(byte);
                hex.push_back(kHexDigits[value >> 4]);
                hex.push_back(kHexDigits[value & 0x0F]);
                ascii.push_back(value >= 0x20 && value < 0x7F ? static_cast<char>(value) : '.');
            }

            return Json {
                {"length",      data.size()},
                {   "hex",   std::move(hex)},
                { "ascii", std::move(ascii)}
            };
        }

        const Json* findArg(const Json& args, std::string_view name)
        {
            if (!args.is_object())
            {
                throw ArgumentError("arguments must be a JSON object");
            }
            const auto found = args.find(name);
            if (found == args.end() || found->is_null())
            {
                return nullptr;
            }
            return &*found;
        }

        bool hasArg(const Json& args, std::string_view name)
        {
            return findArg(args, name) != nullptr;
        }

        std::string requireString(const Json& args, std::string_view name)
        {
            const auto* value = findArg(args, name);
            if (value == nullptr)
            {
                throw ArgumentError("missing required argument '" + std::string {name} + "'");
            }
            if (!value->is_string())
            {
                throw ArgumentError("argument '" + std::string {name} + "' must be a string");
            }
            return value->get<std::string>();
        }

        std::string optionalString(const Json& args, std::string_view name, std::string fallback = {})
        {
            const auto* value = findArg(args, name);
            if (value == nullptr)
            {
                return fallback;
            }
            if (!value->is_string())
            {
                throw ArgumentError("argument '" + std::string {name} + "' must be a string");
            }
            return value->get<std::string>();
        }

        bool optionalBool(const Json& args, std::string_view name, bool fallback = false)
        {
            const auto* value = findArg(args, name);
            if (value == nullptr)
            {
                return fallback;
            }
            if (!value->is_boolean())
            {
                throw ArgumentError("argument '" + std::string {name} + "' must be a boolean");
            }
            return value->get<bool>();
        }

        std::uint64_t decimalFromString(std::string_view text, std::string_view name)
        {
            std::uint64_t parsed {};
            const auto    result = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (result.ec != std::errc {} || result.ptr != text.data() + text.size())
            {
                throw ArgumentError("argument '" + std::string {name} + "' is not a valid integer");
            }
            return parsed;
        }

        // Addresses are hex, with an optional 0x prefix, as the CLI documents them.
        std::uint64_t hexValue(const Json& args, std::string_view name, bool required, std::uint64_t fallback = 0)
        {
            const auto* value = findArg(args, name);
            if (value == nullptr)
            {
                if (required)
                {
                    throw ArgumentError("missing required argument '" + std::string {name} + "'");
                }
                return fallback;
            }
            if (value->is_number_unsigned())
            {
                return value->get<std::uint64_t>();
            }
            if (value->is_number_integer())
            {
                const auto signedValue = value->get<std::int64_t>();
                if (signedValue < 0)
                {
                    throw ArgumentError("argument '" + std::string {name} + "' must not be negative");
                }
                return static_cast<std::uint64_t>(signedValue);
            }
            if (!value->is_string())
            {
                throw ArgumentError("argument '" + std::string {name} + "' must be a hex string or integer");
            }
            const auto parsed = parseHexU64(value->get<std::string>());
            if (!parsed)
            {
                throw ArgumentError("argument '" + std::string {name} + "' is not a valid hex value");
            }
            return *parsed;
        }

        // Counts and offsets are decimal unless written with 0x, like the matching CLI flags.
        std::uint64_t numberValue(const Json& args, std::string_view name, bool required, std::uint64_t fallback = 0)
        {
            const auto* value = findArg(args, name);
            if (value == nullptr)
            {
                if (required)
                {
                    throw ArgumentError("missing required argument '" + std::string {name} + "'");
                }
                return fallback;
            }
            if (value->is_number_unsigned())
            {
                return value->get<std::uint64_t>();
            }
            if (value->is_number_integer())
            {
                const auto signedValue = value->get<std::int64_t>();
                if (signedValue < 0)
                {
                    throw ArgumentError("argument '" + std::string {name} + "' must not be negative");
                }
                return static_cast<std::uint64_t>(signedValue);
            }
            if (!value->is_string())
            {
                throw ArgumentError("argument '" + std::string {name} + "' must be an integer or numeric string");
            }
            const auto text = value->get<std::string>();
            if (text.starts_with("0x") || text.starts_with("0X"))
            {
                const auto parsed = parseHexU64(text);
                if (!parsed)
                {
                    throw ArgumentError("argument '" + std::string {name} + "' is not a valid hex value");
                }
                return *parsed;
            }
            return decimalFromString(text, name);
        }

        std::uint32_t u32Value(const Json&      args,
                               std::string_view name,
                               bool             required,
                               std::uint32_t    fallback = 0,
                               std::uint32_t    maximum  = std::numeric_limits<std::uint32_t>::max())
        {
            const auto value = numberValue(args, name, required, fallback);
            if (value > maximum)
            {
                throw ArgumentError("argument '" + std::string {name} + "' must be at most " + std::to_string(maximum));
            }
            return static_cast<std::uint32_t>(value);
        }

        std::uint32_t pidValue(const Json& args, bool required)
        {
            const auto* value = findArg(args, "pid");
            if (value == nullptr)
            {
                if (required)
                {
                    throw ArgumentError("missing required argument 'pid'");
                }
                return 0;
            }
            if (value->is_number_integer() && value->get<std::int64_t>() == -1)
            {
                return kPhysicalPid;
            }
            if (value->is_string() && value->get<std::string>() == "-1")
            {
                return kPhysicalPid;
            }
            return u32Value(args, "pid", required);
        }

        std::uint32_t readSize(const Json& args, std::uint32_t fallback)
        {
            const auto* value = findArg(args, "size");
            if (value == nullptr)
            {
                return fallback;
            }
            std::uint32_t size {};
            if (value->is_number_unsigned())
            {
                const auto parsed = value->get<std::uint64_t>();
                if (parsed > std::numeric_limits<std::uint32_t>::max())
                {
                    throw ArgumentError("argument 'size' is too large");
                }
                size = static_cast<std::uint32_t>(parsed);
            }
            else if (value->is_string())
            {
                const auto parsed = parseRangeSize(value->get<std::string>());
                if (!parsed)
                {
                    throw ArgumentError("argument 'size' is not a valid size");
                }
                size = *parsed;
            }
            else
            {
                throw ArgumentError("argument 'size' must be an integer or numeric string");
            }

            if (size == 0)
            {
                throw ArgumentError("argument 'size' must be at least one byte");
            }
            if (size > kMaxReadSizeMcp)
            {
                throw ArgumentError("argument 'size' must be at most " + std::to_string(kMaxReadSizeMcp) + " bytes");
            }
            return size;
        }

        std::vector<std::byte> dataArg(const Json& args)
        {
            const auto text = requireString(args, "data");
            const auto data = parseHexBytes(text);
            if (!data || data->empty())
            {
                throw ArgumentError("argument 'data' must be a non-empty hex byte string");
            }
            return *data;
        }

        std::vector<PageRange> rangeArg(const Json& args, std::string_view name, bool required)
        {
            const auto* value = findArg(args, name);
            if (value == nullptr)
            {
                if (required)
                {
                    throw ArgumentError("missing required argument '" + std::string {name} + "'");
                }
                return {};
            }
            if (!value->is_array() || value->empty())
            {
                throw ArgumentError("argument '" + std::string {name} + "' must be a non-empty array of ranges");
            }

            std::vector<PageRange> ranges;
            ranges.reserve(value->size());
            for (const auto& item : *value)
            {
                if (!item.is_object())
                {
                    throw ArgumentError("each range must be an object with address and size");
                }
                const Json wrapper = {
                    {"range", item}
                };
                const auto address = hexValue(wrapper["range"], "address", true);
                const auto size    = numberValue(wrapper["range"], "size", true);
                if (size == 0)
                {
                    throw ArgumentError("a range size must be at least one byte");
                }
                ranges.push_back({address, size});
            }
            return ranges;
        }

        struct Pagination
        {
            std::size_t offset {};
            std::size_t limit {kDefaultListLimit};
        };

        Pagination pagination(const Json& args)
        {
            Pagination page;
            page.offset = static_cast<std::size_t>(numberValue(args, "offset", false, 0));
            page.limit  = static_cast<std::size_t>(numberValue(args, "limit", false, kDefaultListLimit));
            if (page.limit == 0 || page.limit > kMaxListLimit)
            {
                throw ArgumentError("argument 'limit' must be between 1 and " + std::to_string(kMaxListLimit));
            }
            return page;
        }

        template<typename T, typename Mapper>
        Json listResult(const std::vector<T>& items, const Pagination& page, bool upstreamTruncated, Mapper&& mapper)
        {
            const auto total = items.size();
            const auto start = std::min(page.offset, total);
            const auto end   = std::min(start + page.limit, total);

            Json entries = Json::array();
            for (auto index = start; index < end; ++index)
            {
                entries.push_back(mapper(items[index]));
            }

            return Json {
                {    "items",               std::move(entries)},
                {    "count",                      end - start},
                {    "total",                            total},
                {   "offset",                            start},
                {    "limit",                       page.limit},
                {"truncated", upstreamTruncated || end < total}
            };
        }

        Json targetJson(const TargetDescriptor& target)
        {
            return Json {
                {      "name",          target.name},
                {  "qemu_pid",       target.qemuPid},
                {"qmp_socket", target.qmpSocketPath}
            };
        }

        Json processJson(const ProcessDescriptor& process)
        {
            return Json {
                {       "pid",       process.pid},
                {"parent_pid", process.parentPid},
                {      "name",      process.name},
                {     "wow64",     process.wow64}
            };
        }

        ToolOutcome success(Json structured)
        {
            ToolOutcome outcome;
            outcome.structuredJson = structured.dump();
            outcome.text           = structured.dump(2);
            return outcome;
        }

        ToolOutcome failure(std::string message)
        {
            ToolOutcome outcome;
            outcome.isError = true;
            outcome.text    = std::move(message);
            return outcome;
        }

        const std::vector<ToolSpec>& toolSpecs()
        {
            const auto target = ParamSpec {"target", "string", "VM target name, as returned by list_targets", true};
            const auto pid    = ParamSpec {"pid", "integer", "Process id", true};
            const auto optPid = ParamSpec {"pid", "integer", "Process id; omit to select every process", false};
            const auto limit = ParamSpec {"limit", "integer", "Maximum entries to return (1-1000, default 100)", false};
            const auto offset = ParamSpec {"offset", "integer", "Entries to skip before returning results", false};

            static const std::vector<ToolSpec> specs = {
                {         "list_targets","List VM targets",                                                 "List the QEMU/KVM guests the host can serve.",true,                                                                                              {}                      },
                {         "capabilities",
                 "Host capabilities",                                      "Show the host version and the request types it handles.",
                 true,                                                                                                    {}},
                {          "bind_target",
                 "Bind to a VM target",             "Bind a connection to one VM target and report the resulting session. Most other tools bind "
             "implicitly, so this is only needed to inspect the target record.", true,
                 {target}                                                                                                   },
                {       "list_processes",
                 "List processes",                       "List the processes of a VM target, optionally filtered by pid or name.",
                 true, {target, optPid, {"name", "string", "Case-insensitive process name substring", false}, limit, offset}},
                {   "list_all_processes",
                 "List all processes",                               "List every process, including terminated ones, with its state.",
                 true, {target, optPid, {"name", "string", "Case-insensitive process name substring", false}, limit, offset}},
                {         "process_info",
                 "Process information",                 "Describe one process. Give either pid or name; name is resolved by the host.",
                 true,             {target,
             {"pid", "integer", "Process id, or 0 when name is used", false},
             {"name", "string", "Process name to resolve when pid is absent", false}}                 },
                {         "list_modules",
                 "List modules",                                                        "List the loaded modules of a process.",
                 true,                                                                              {target,
                                                                              pid,
                                                                              {"name", "string", "Case-insensitive module name substring", false},
                                                                              {"counts", "boolean", "Include raw size and section/EAT/IAT counts", false},
                                                                              {"debug", "boolean", "Include PDB GUID and filename", false},
                                                                              {"version_info", "boolean", "Include the file version string", false},
                                                                              limit,
                                                                              offset}                 },
                {        "lookup_symbol",
                 "Look up a symbol",                          "Resolve a module base and, when symbol is given, an export address.",
                 true,     {target,
     pid,
     {"module", "string", "Module name", true},
     {"symbol", "string", "Export name; omit to return the module base only", false}}                 },
                {             "pdb_load",
                 "Load a PDB",                     "Load the PDB of the module at a base address and report its module name.",
                 true,                               {target, pid, {"address", "string", "Module base address (hex)", true}}},
                {      "pdb_symbol_name",
                 "PDB symbol name",                                    "Resolve an address to a PDB symbol name and displacement.",
                 true,              {target,
              {"module", "string", "Module name", true},
              {"address", "string", "Address or module-relative offset (hex)", true}}                 },
                {   "pdb_symbol_address",
                 "PDB symbol address",                                                     "Resolve a PDB symbol name to an address.",
                 true,        {target, {"module", "string", "Module name", true}, {"symbol", "string", "Symbol name", true}}},
                {        "pdb_type_size",
                 "PDB type size",                                                            "Show the byte size of a PDB type.",
                 true,            {target, {"module", "string", "Module name", true}, {"type", "string", "Type name", true}}},
                {"pdb_type_child_offset",
                 "PDB child offset",                                        "Show the byte offset of a child member in a PDB type.",
                 true,                                      {target,
                                      {"module", "string", "Module name", true},
                                      {"type", "string", "Type name", true},
                                      {"child", "string", "Child member name", true}}                 },
                {        "module_layout",
                 "Module layout",                                        "List a module's sections and its 16 data directories.",
                 true,                                              {target, pid, {"module", "string", "Module name", true}}},
                {           "memory_map",
                 "Memory map",                                    "List the virtual address descriptors (VADs) of a process.",
                 true,                                                                              {target,
                                                                              pid,
                                                                              {"identify_modules", "boolean", "Name the owning module in each description (slower)", false},
                                                                              limit,
                                                                              offset}                 },
                {               "vad_ex",
                 "VAD page map",                                                   "Describe the pages of a process's VAD map.",
                 true,                                                                              {target,
                                                                              pid,
                                                                              {"page_offset", "integer", "Offset in pages from the process start", false},
                                                                              {"page_count", "integer", "Number of pages to describe (1-4096, default 1)", false},
                                                                              limit,
                                                                              offset}                 },
                {         "list_threads",
                 "List threads",                                                               "List the threads of a process.",
                 true,                                                                              {target,
                                                                              pid,
                                                                              {"tid", "integer", "Only return this thread id", false},
                                                                              {"callstack", "boolean", "Include each thread's user-mode callstack (slower)", false},
                                                                              limit,
                                                                              offset}                 },
                {         "list_handles",
                 "List handles",                                                        "List the kernel handles of a process.",
                 true,  {target, pid, {"filter", "string", "Case-insensitive name or type substring", false}, limit, offset}},
                {         "list_exports",
                 "List exports",                                                           "List the export table of a module.",
                 true,                               {target, pid, {"module", "string", "Module name", true}, limit, offset}},
                {         "list_imports",
                 "List imports",                                                           "List the import table of a module.",
                 true,                               {target, pid, {"module", "string", "Module name", true}, limit, offset}},
                {"list_unloaded_modules",
                 "List unloaded modules",                                                      "List the unloaded modules of a process.",
                 true,          {target, pid, {"filter", "string", "Case-insensitive name substring", false}, limit, offset}},
                {        "list_services",
                 "List services",                                                                  "List the target's services.",
                 true,                                                                              {target,
                                                                              {"filter", "string", "Service name, display name, image path or account", false},
                                                                              limit,
                                                                              offset}                 },
                {           "list_users",
                 "List users",                           "List the target's local accounts with their SID and registry hive.",
                 true,                 {target, {"filter", "string", "Account name or SID substring", false}, limit, offset}},
                {         "list_network",
                 "List network connections",                             "List the target's network connections, optionally scoped by pid.",
                 true,    {target, optPid, {"filter", "string", "Process name or endpoint substring", false}, limit, offset}},
                {           "list_heaps",
                 "List heaps",                    "List the heaps of a process and the number list_heap_allocations selects.",
                 true,                                                                          {target, pid, limit, offset}},
                {"list_heap_allocations",
                 "List heap allocations",                                               "List the allocations of one heap of a process.",
                 true,          {target, pid, {"heap", "integer", "Heap number or heap address (hex)", true}, limit, offset}},
                {  "list_kernel_drivers",
                 "List kernel drivers",                                                            "List the target's kernel drivers.",
                 true,    {target, {"filter", "string", "Driver name, path or service key substring", false}, limit, offset}},
                {  "list_kernel_devices",
                 "List kernel devices",                                                     "List the target's kernel device objects.",
                 true,          {target, {"filter", "string", "Device type or volume info substring", false}, limit, offset}},
                {  "list_kernel_objects",
                 "List kernel objects",                                                   "List the target's kernel object namespace.",
                 true,                 {target, {"filter", "string", "Object name or type substring", false}, limit, offset}},
                {             "list_pte",
                 "List page-table regions",                "List a process's page-table regions. pid 0 describes the kernel's own tables.",
                 true,                                                                              {target,
                                                                              pid,
                                                                              {"identify_modules", "boolean", "Name the owning module (slow)", false},
                                                                              {"filter", "string", "Case-insensitive identified text substring", false},
                                                                              {"min_address", "string", "Lowest address of the window (hex)", false},
                                                                              {"max_address", "string", "Exclusive highest address of the window (hex)", false},
                                                                              limit,
                                                                              offset}                 },
                {            "list_pool",
                 "List kernel pool",                                                                "List kernel pool allocations.",
                 true,                                                                              {target,
                                                                              {"big", "boolean", "Report the big-pool table instead of the whole pool", false},
                                                                              {"tag", "string", "Case-insensitive prefix of the four-character pool tag", false},
                                                                              {"min_bytes", "integer", "Minimum allocation size", false},
                                                                              {"min_address", "string", "Lowest address of the window (hex)", false},
                                                                              {"max_address", "string", "Exclusive highest address of the window (hex)", false},
                                                                              limit,
                                                                              offset}                 },
                {       "list_pool_tags",
                 "Summarize pool tags",                                                   "Summarize kernel pool allocations per tag.",
                 true,                                                                              {target,
                                                                              {"big", "boolean", "Summarize the big-pool table instead of the whole pool", false},
                                                                              {"tag", "string", "Case-insensitive prefix of the four-character pool tag", false},
                                                                              limit,
                                                                              offset}                 },
                {             "list_pfn",
                 "List page frames",                                                             "Describe a range of page frames.",
                 true,                                                                              {target,
                                                                              {"pfn_start", "integer", "First page frame number", true},
                                                                              {"pfn_count", "integer", "Number of frames (1-32768, default 1)", false},
                                                                              limit,
                                                                              offset}                 },
                {             "list_vfs",
                 "List VFS directory",                                       "List a directory of the MemProcFS virtual file system.",
                 true,                                                                              {target,
                                                                              {"path", "string", "VFS path; '/' and '\\\\' both separate components (default root)", false},
                                                                              {"name", "string", "Case-insensitive name substring", false},
                                                                              limit,
                                                                              offset}                 },
                {             "read_vfs",
                 "Read VFS file",                                                              "Read bytes from a virtual file.",
                 true,           {target,
           {"path", "string", "VFS path", true},
           {"offset", "string", "Byte offset to read from (hex, default 0)", false},
           {"size", "integer", "Bytes to read (default 4096, maximum 1 MiB)", false}}                 },
                {          "read_memory",
                 "Read process memory",                       "Read process virtual memory, returning hex bytes and an ASCII preview.",
                 true,            {target,
            pid,
            {"address", "string", "Virtual address (hex)", true},
            {"size", "integer", "Bytes to read (default 256, maximum 1 MiB)", false}}                 },
                {         "scatter_read",
                 "Scatter read",                                     "Read several process memory ranges in one scatter batch.",
                 true,           {target,
           pid,
           {"ranges", "array", "Array of {address (hex), size} objects", true},
           {"repeat", "integer", "Read the prepared ranges this many times (default 1)", false},
           {"clear", "boolean", "Clear the scatter handle before closing it", false}}                 },
                {             "list_vms",
                 "List guest partitions",                      "List the guest partitions of a VM target; index 0 is the target itself.",
                 true,                                                                               {target, limit, offset}},
                { "read_physical_memory",
                 "Read physical memory",                 "Read guest physical memory by GPA, returning hex bytes and an ASCII preview.",
                 true,            {target,
            {"vm_index", "integer", "Guest partition index from list_vms (default 0)", false},
            {"address", "string", "Guest physical address (hex)", true},
            {"size", "integer", "Bytes to read (default 256, maximum 1 MiB)", false}}                 },
                {        "translate_gpa",
                 "Translate GPA",         "Translate a guest physical address to a physical address or a vmmem virtual address.",
                 true,                         {target,
                         {"vm_index", "integer", "Guest partition index from list_vms (default 0)", false},
                         {"address", "string", "Guest physical address (hex)", true}}                 },
                {            "virt2phys",
                 "Translate virtual address",                                   "Translate a process virtual address to a physical address.",
                 true,                                   {target, pid, {"address", "string", "Virtual address (hex)", true}}},
                {     "physical_scatter",
                 "Physical scatter read",                                     "Read several guest physical ranges in one scatter batch.",
                 true,                 {target,
                 {"vm_index", "integer", "Guest partition index from list_vms (default 0)", false},
                 {"ranges", "array", "Array of {address (hex), size} objects", true}}                 },
                {        "query_virtual",
                 "Query virtual address",                                    "Describe the region a process virtual address belongs to.",
                 true,                                   {target, pid, {"address", "string", "Virtual address (hex)", true}}},
                {           "read_pages",
                 "Read pages",                              "Read whole pages of a process (pid -1 selects physical memory).",
                 true,                    {target, pid, {"ranges", "array", "Array of {address (hex), size} objects", true}}},
                {       "prefetch_pages",
                 "Prefetch pages",               "Warm the page cache for a process range list (pid -1 selects physical memory).",
                 true,                    {target, pid, {"ranges", "array", "Array of {address (hex), size} objects", true}}},
                {        "search_memory",
                 "Search memory",            "Search process memory (pid -1 selects physical memory) for a text or hex pattern.",
                 true,                                                                              {target,
                                                                              pid,
                                                                              {"pattern", "string", "Literal text to search for (use hex_pattern for wildcards)", false},
                                                                              {"hex_pattern", "string", "Hex bytes with ?? wildcards", false},
                                                                              {"min_address", "string", "Lowest address (hex, default 0)", false},
                                                                              {"max_address", "string", "Exclusive highest address (hex, default end)", false},
                                                                              {"align", "integer", "Hit alignment, a power of two (default 1)", false},
                                                                              {"max_results", "integer", "Stop after this many hits (default 1024)", false},
                                                                              limit,
                                                                              offset}                 },
                // Write tools are hidden unless --mcp-allow-write is given.
                {           "config_set",
                 "Set host configuration", "Set an allow-listed MemProcFS cache/tick, refresh or process-DTB option. Changes host state.",
                 false,    {target,
    {"option", "string", "Config option name", true},
    {"pid", "integer", "Process id for process-scoped options", false},
    {"value", "integer", "Decimal or 0x-hex value for options that take one", false}}                },
                {         "write_memory",
                 "Write process memory",                    "Write bytes into process virtual memory. Destructive: modifies the guest.",
                 false,                          {target,
                          pid,
                          {"address", "string", "Virtual address (hex)", true},
                          {"data", "string", "Bytes to write as a hex string", true}}                },
                {            "write_vfs",
                 "Write VFS file",     "Write bytes to a virtual file. Destructive and still needs the host's --allow-vfs-write.",
                 false,                          {target,
                          {"path", "string", "VFS path", true},
                          {"offset", "string", "Byte offset to write at (hex, default 0)", false},
                          {"data", "string", "Bytes to write as a hex string", true}}                },
                {"write_physical_memory",
                 "Write physical memory",                     "Write bytes into guest physical memory. Destructive: modifies the guest.",
                 false,                          {target,
                          {"vm_index", "integer", "Guest partition index from list_vms (default 0)", false},
                          {"address", "string", "Guest physical address (hex)", true},
                          {"data", "string", "Bytes to write as a hex string", true}}                },
            };
            return specs;
        }

        Json toolSchema(const ToolSpec& spec)
        {
            Json properties = Json::object();
            Json required   = Json::array();
            for (const auto& param : spec.params)
            {
                Json property = {
                    {       "type",        param.type},
                    {"description", param.description}
                };
                if (std::string_view {param.type} == "array")
                {
                    property["items"] = Json {
                        {"type", "object"}
                    };
                }
                properties[param.name] = std::move(property);
                if (param.required)
                {
                    required.push_back(param.name);
                }
            }

            return Json {
                {      "type",              "object"},
                {"properties", std::move(properties)},
                {  "required",   std::move(required)}
            };
        }

        const ToolSpec* findSpec(std::string_view name)
        {
            const auto& specs = toolSpecs();
            const auto  found = std::find_if(specs.begin(),
                                             specs.end(),
                                             [name](const ToolSpec& spec)
                                             {
                                                return name == spec.name;
                                             });
            return found == specs.end() ? nullptr : &*found;
        }
    } // namespace

    std::vector<ToolInfo> toolInfos(bool allowWrite)
    {
        std::vector<ToolInfo> infos;
        for (const auto& spec : toolSpecs())
        {
            if (!spec.readOnly && !allowWrite)
            {
                continue;
            }
            infos.push_back({spec.name, spec.readOnly});
        }
        return infos;
    }

    bool isWriteTool(std::string_view name)
    {
        const auto* spec = findSpec(name);
        return spec != nullptr && !spec->readOnly;
    }

    std::string toolListJson(bool allowWrite)
    {
        Json tools = Json::array();
        for (const auto& spec : toolSpecs())
        {
            if (!spec.readOnly && !allowWrite)
            {
                continue;
            }
            Json tool = {
                {       "name",spec.name                               },
                {      "title",       spec.title},
                {"description", spec.description},
                {"inputSchema", toolSchema(spec)},
                {"annotations",
                 {{"title", spec.title},
                 {"readOnlyHint", spec.readOnly},
                 {"destructiveHint", !spec.readOnly},
                 {"idempotentHint", spec.readOnly},
                 {"openWorldHint", false}}      }
            };
            tools.push_back(std::move(tool));
        }
        return tools.dump();
    }

    ToolOutcome callTool(std::string_view name, std::string_view argumentsJson, const ToolContext& context)
    {
        const auto* spec = findSpec(name);
        if (spec == nullptr)
        {
            return failure("unknown tool '" + std::string {name} + "'");
        }
        if (!spec->readOnly && !context.allowWrite)
        {
            return failure("tool '" + std::string {name}
                           + "' is disabled; start the MCP server with --mcp-allow-write to enable write tools");
        }

        try
        {
            Json args = Json::object();
            if (!argumentsJson.empty())
            {
                args = Json::parse(argumentsJson);
            }
            if (!args.is_object())
            {
                throw ArgumentError("arguments must be a JSON object");
            }

            const auto& socket = context.socketPath;
            std::string error;

            if (name == "list_targets")
            {
                const auto response = listTargets(socket, error);
                if (!response)
                {
                    return failure(error);
                }
                Json targets = Json::array();
                for (const auto& target : response->targets)
                {
                    targets.push_back(targetJson(target));
                }
                return success(Json {
                    {"targets", std::move(targets)}
                });
            }

            if (name == "capabilities")
            {
                const auto response = getCapabilities(socket, error);
                if (!response)
                {
                    return failure(error);
                }
                Json types = Json::array();
                for (const auto type : response->requestTypes)
                {
                    types.push_back(Json {
                        { "name", messageTypeName(static_cast<MessageType>(type))},
                        {"value",                                  hexSmall(type)}
                    });
                }
                return success(Json {
                    {"server_version", response->serverVersion},
                    { "request_types",        std::move(types)}
                });
            }

            if (name == "bind_target")
            {
                const auto response = bindTarget(socket, requireString(args, "target"), error);
                if (!response)
                {
                    return failure(error);
                }
                return success(Json {
                    {"session_id", hexString(response->sessionId)},
                    {    "target",   targetJson(response->target)}
                });
            }

            if (name == "list_processes")
            {
                const auto page = pagination(args);
                const auto response =
                    listProcesses(socket,
                                  {requireString(args, "target"), pidValue(args, false), optionalString(args, "name")},
                                  error);
                if (!response)
                {
                    return failure(error);
                }
                return success(listResult(response->processes,
                                          page,
                                          false,
                                          [](const ProcessDescriptor& process)
                                          {
                                              return processJson(process);
                                          }));
            }

            if (name == "list_all_processes")
            {
                const auto page     = pagination(args);
                const auto response = listAllProcesses(
                    socket,
                    {requireString(args, "target"), pidValue(args, false), optionalString(args, "name")},
                    error);
                if (!response)
                {
                    return failure(error);
                }
                return success(listResult(response->processes,
                                          page,
                                          false,
                                          [](const ProcessSummary& summary)
                                          {
                                              auto entry     = processJson(summary.process);
                                              entry["state"] = summary.state;
                                              return entry;
                                          }));
            }

            if (name == "process_info")
            {
                const auto pid  = pidValue(args, false);
                const auto text = optionalString(args, "name");
                if (pid == 0 && text.empty())
                {
                    throw ArgumentError("process_info needs 'pid' or 'name'");
                }
                const auto response = processInfo(socket, {requireString(args, "target"), pid, text}, error);
                if (!response)
                {
                    return failure(error);
                }
                return success(Json {
                    {        "process",  processJson(response->process)},
                    {          "state",                 response->state},
                    {   "memory_model",           response->memoryModel},
                    {    "system_type",            response->systemType},
                    {      "user_only",              response->userOnly},
                    {            "dtb",        hexString(response->dtb)},
                    {       "dtb_user",    hexString(response->dtbUser)},
                    {       "eprocess", hexString(response->eprocessVa)},
                    {            "peb",      hexString(response->pebVa)},
                    {          "peb32",    hexString(response->peb32Va)},
                    {     "session_id",             response->sessionId},
                    {           "luid",       hexString(response->luid)},
                    {"integrity_level",        response->integrityLevel},
                    {            "sid",                   response->sid},
                    {     "image_path",             response->imagePath},
                    {    "kernel_path",            response->kernelPath},
                    {        "cmdline",               response->cmdline},
                });
            }

            if (name == "list_modules")
            {
                const auto    page  = pagination(args);
                std::uint16_t flags = 0;
                if (optionalBool(args, "counts"))
                {
                    flags |= kModuleFlagCounts;
                }
                if (optionalBool(args, "debug"))
                {
                    flags |= kModuleFlagDebugInfo;
                }
                if (optionalBool(args, "version_info"))
                {
                    flags |= kModuleFlagVersionInfo;
                }
                const auto response = listModules(
                    socket,
                    {requireString(args, "target"), u32Value(args, "pid", true), optionalString(args, "name"), flags},
                    error);
                if (!response)
                {
                    return failure(error);
                }
                return success(listResult(*response,
                                          page,
                                          false,
                                          [](const ModuleDescriptor& module)
                                          {
                                              return Json {
                                                  {         "base",  hexString(module.base)},
                                                  {        "entry", hexString(module.entry)},
                                                  {   "image_size",        module.imageSize},
                                                  {         "kind",             module.kind},
                                                  {        "wow64",            module.wow64},
                                                  {         "name",             module.name},
                                                  {    "full_name",         module.fullName},
                                                  {"raw_file_size",      module.rawFileSize},
                                                  {"section_count",     module.sectionCount},
                                                  {    "eat_count",         module.eatCount},
                                                  {    "iat_count",         module.iatCount},
                                                  {     "pdb_guid",          module.pdbGuid},
                                                  {     "pdb_name",          module.pdbName},
                                                  {      "version",          module.version}
                                              };
                                          }));
            }

            if (name == "lookup_symbol")
            {
                const auto response = lookupSymbol(socket,
                                                   {requireString(args, "target"),
                                                    u32Value(args, "pid", true),
                                                    requireString(args, "module"),
                                                    optionalString(args, "symbol")},
                                                   error);
                if (!response)
                {
                    return failure(error);
                }
                return success(Json {
                    {"module_base", hexString(response->moduleBase)},
                    {    "address",    hexString(response->address)},
                    {     "offset",     hexString(response->offset)},
                    {      "found",                 response->found}
                });
            }

            if (name == "pdb_load")
            {
                const auto response = pdbLoad(
                    socket,
                    {requireString(args, "target"), u32Value(args, "pid", true), hexValue(args, "address", true)},
                    error);
                if (!response)
                {
                    return failure(error);
                }
                return success(Json {
                    {"module", response->module}
                });
            }

            if (name == "pdb_symbol_name")
            {
                const auto response = pdbSymbolName(
                    socket,
                    {requireString(args, "target"), requireString(args, "module"), hexValue(args, "address", true)},
                    error);
                if (!response)
                {
                    return failure(error);
                }
                return success(Json {
                    {      "symbol",                 response->symbol},
                    {"displacement", hexSmall(response->displacement)}
                });
            }

            if (name == "pdb_symbol_address")
            {
                const auto response = pdbSymbolAddress(
                    socket,
                    {requireString(args, "target"), requireString(args, "module"), requireString(args, "symbol")},
                    error);
                if (!response)
                {
                    return failure(error);
                }
                return success(Json {
                    {"address", hexString(response->address)}
                });
            }

            if (name == "pdb_type_size")
            {
                const auto response = pdbTypeSize(
                    socket,
                    {requireString(args, "target"), requireString(args, "module"), requireString(args, "type")},
                    error);
                if (!response)
                {
                    return failure(error);
                }
                return success(Json {
                    {"size", response->size}
                });
            }

            if (name == "pdb_type_child_offset")
            {
                const auto response = pdbTypeChildOffset(socket,
                                                         {requireString(args, "target"),
                                                          requireString(args, "module"),
                                                          requireString(args, "type"),
                                                          requireString(args, "child")},
                                                         error);
                if (!response)
                {
                    return failure(error);
                }
                return success(Json {
                    {"offset", hexSmall(response->offset)}
                });
            }

            if (name == "module_layout")
            {
                const auto response = moduleLayout(
                    socket,
                    {requireString(args, "target"), u32Value(args, "pid", true), requireString(args, "module")},
                    error);
                if (!response)
                {
                    return failure(error);
                }
                Json sections = Json::array();
                for (const auto& section : response->sections)
                {
                    sections.push_back(Json {
                        {           "name",                      section.name},
                        {"virtual_address", hexString(section.virtualAddress)},
                        {   "virtual_size",               section.virtualSize},
                        {    "raw_pointer",                section.rawPointer},
                        {       "raw_size",                   section.rawSize},
                        {"characteristics", hexSmall(section.characteristics)}
                    });
                }
                Json directories = Json::array();
                for (const auto& directory : response->directories)
                {
                    directories.push_back(Json {
                        {          "index",                    directory.index},
                        {"virtual_address", hexSmall(directory.virtualAddress)},
                        {           "size",                     directory.size}
                    });
                }
                return success(Json {
                    {"module_base", hexString(response->moduleBase)},
                    {   "sections",             std::move(sections)},
                    {"directories",          std::move(directories)}
                });
            }

            if (name == "memory_map")
            {
                const auto page     = pagination(args);
                const auto response = listMemoryMap(socket,
                                                    {requireString(args, "target"),
                                                     u32Value(args, "pid", true),
                                                     optionalBool(args, "identify_modules")},
                                                    error);
                if (!response)
                {
                    return failure(error);
                }
                return success(listResult(*response,
                                          page,
                                          false,
                                          [](const MemoryMapRegion& region)
                                          {
                                              return Json {
                                                  {        "start",     hexString(region.start)},
                                                  {          "end",       hexString(region.end)},
                                                  {   "protection", hexSmall(region.protection)},
                                                  {"commit_charge",         region.commitCharge},
                                                  {     "vad_type",              region.vadType},
                                                  {        "flags",      hexSmall(region.flags)},
                                                  {  "heap_number",           region.heapNumber},
                                                  {  "description",          region.description}
                                              };
                                          }));
            }

            if (name == "vad_ex")
            {
                const auto page     = pagination(args);
                const auto response = listVadEx(socket,
                                                {requireString(args, "target"),
                                                 u32Value(args, "pid", true),
                                                 u32Value(args, "page_offset", false, 0),
                                                 u32Value(args, "page_count", false, 1, kMaxPageCount)},
                                                error);
                if (!response)
                {
                    return failure(error);
                }
                return success(listResult(*response,
                                          page,
                                          false,
                                          [](const VadExDescriptor& entry)
                                          {
                                              return Json {
                                                  {      "type",                 entry.type},
                                                  {       "pml",                  entry.pml},
                                                  {     "flags",      hexSmall(entry.flags)},
                                                  {        "va",        hexString(entry.va)},
                                                  {        "pa",        hexString(entry.pa)},
                                                  {       "pte",       hexString(entry.pte)},
                                                  {"proto_type",            entry.protoType},
                                                  {  "proto_pa",   hexString(entry.protoPa)},
                                                  { "proto_pte",  hexString(entry.protoPte)},
                                                  {  "vad_base", hexString(entry.vaVadBase)}
                                              };
                                          }));
            }

            if (name == "list_threads")
            {
                const auto    page  = pagination(args);
                std::uint16_t flags = 0;
                if (optionalBool(args, "callstack"))
                {
                    flags |= kThreadFlagCallstack;
                }
                const auto response = listThreads(
                    socket,
                    {requireString(args, "target"), u32Value(args, "pid", true), u32Value(args, "tid", false), flags},
                    error);
                if (!response)
                {
                    return failure(error);
                }
                return success(listResult(*response,
                                          page,
                                          false,
                                          [](const ThreadDescriptor& thread)
                                          {
                                              Json frames = Json::array();
                                              for (const auto& frame : thread.frames)
                                              {
                                                  frames.push_back(Json {
                                                      {            "index",                    frame.index},
                                                      {   "return_address", hexString(frame.returnAddress)},
                                                      {    "stack_pointer",  hexString(frame.stackPointer)},
                                                      {       "frame_base",     hexString(frame.frameBase)},
                                                      {     "displacement",   hexSmall(frame.displacement)},
                                                      {"registers_present",         frame.registersPresent},
                                                      {           "module",                   frame.module},
                                                      {         "function",                 frame.function}
                                                  });
                                              }
                                              return Json {
                                                  {                "tid",                           thread.tid},
                                                  {                "pid",                           thread.pid},
                                                  {        "exit_status",                    thread.exitStatus},
                                                  {          "user_time",                      thread.userTime},
                                                  {        "kernel_time",                    thread.kernelTime},
                                                  {            "ethread",            hexString(thread.ethread)},
                                                  {                "teb",                hexString(thread.teb)},
                                                  {      "start_address",       hexString(thread.startAddress)},
                                                  {"win32_start_address",  hexString(thread.win32StartAddress)},
                                                  {    "stack_base_user",      hexString(thread.stackBaseUser)},
                                                  {   "stack_limit_user",     hexString(thread.stackLimitUser)},
                                                  {  "stack_base_kernel",    hexString(thread.stackBaseKernel)},
                                                  { "stack_limit_kernel",   hexString(thread.stackLimitKernel)},
                                                  {         "trap_frame",          hexString(thread.trapFrame)},
                                                  {"instruction_pointer", hexString(thread.instructionPointer)},
                                                  {      "stack_pointer",       hexString(thread.stackPointer)},
                                                  {           "affinity",           hexString(thread.affinity)},
                                                  {        "create_time",         hexString(thread.createTime)},
                                                  {          "exit_time",           hexString(thread.exitTime)},
                                                  {"impersonation_token", hexString(thread.impersonationToken)},
                                                  {              "state",                         thread.state},
                                                  {            "running",                       thread.running},
                                                  {           "priority",                      thread.priority},
                                                  {      "base_priority",                  thread.basePriority},
                                                  {      "suspend_count",                  thread.suspendCount},
                                                  {        "wait_reason",                    thread.waitReason},
                                                  {   "callstack_status",               thread.callstackStatus},
                                                  {             "frames",                    std::move(frames)}
                                              };
                                          }));
            }

            if (name == "list_handles")
            {
                const auto page     = pagination(args);
                const auto response = listHandles(
                    socket,
                    {requireString(args, "target"), u32Value(args, "pid", true), optionalString(args, "filter")},
                    error);
                if (!response)
                {
                    return failure(error);
                }
                auto result = listResult(*response,
                                         page,
                                         false,
                                         [](const HandleDescriptor& handle)
                                         {
                                             return Json {
                                                 {          "va_object",           hexString(handle.vaObject)},
                                                 {             "handle",                        handle.handle},
                                                 {     "granted_access",       hexSmall(handle.grantedAccess)},
                                                 {         "type_index",                     handle.typeIndex},
                                                 {       "handle_count",        hexString(handle.handleCount)},
                                                 {      "pointer_count",       hexString(handle.pointerCount)},
                                                 { "object_create_info",   hexString(handle.objectCreateInfo)},
                                                 {"security_descriptor", hexString(handle.securityDescriptor)},
                                                 {           "pool_tag",          poolTagText(handle.poolTag)},
                                                 {               "text",                          handle.text},
                                                 {               "type",                          handle.type}
                                             };
                                         });
                return success(std::move(result));
            }

            if (name == "list_exports")
            {
                const auto page     = pagination(args);
                const auto response = listExports(
                    socket,
                    {requireString(args, "target"), u32Value(args, "pid", true), requireString(args, "module")},
                    error);
                if (!response)
                {
                    return failure(error);
                }
                return success(listResult(*response,
                                          page,
                                          false,
                                          [](const ExportDescriptor& entry)
                                          {
                                              return Json {
                                                  {           "va_function",          hexString(entry.vaFunction)},
                                                  {               "ordinal",                        entry.ordinal},
                                                  {"functions_array_offset", hexSmall(entry.functionsArrayOffset)},
                                                  {    "names_array_offset",     hexSmall(entry.namesArrayOffset)},
                                                  {              "function",                       entry.function},
                                                  {    "forwarded_function",              entry.forwardedFunction}
                                              };
                                          }));
            }

            if (name == "list_imports")
            {
                const auto page     = pagination(args);
                const auto response = listImports(
                    socket,
                    {requireString(args, "target"), u32Value(args, "pid", true), requireString(args, "module")},
                    error);
                if (!response)
                {
                    return failure(error);
                }
                return success(listResult(*response,
                                          page,
                                          false,
                                          [](const ImportDescriptor& entry)
                                          {
                                              return Json {
                                                  {             "va_function",           hexString(entry.vaFunction)},
                                                  {               "is_32_bit",                         entry.is32Bit},
                                                  {                    "hint",                            entry.hint},
                                                  {         "first_thunk_rva",         hexSmall(entry.firstThunkRva)},
                                                  {"original_first_thunk_rva", hexSmall(entry.originalFirstThunkRva)},
                                                  {                "function",                        entry.function},
                                                  {                  "module",                          entry.module}
                                              };
                                          }));
            }

            if (name == "list_unloaded_modules")
            {
                const auto page     = pagination(args);
                const auto response = listUnloadedModules(
                    socket,
                    {requireString(args, "target"), u32Value(args, "pid", true), optionalString(args, "filter")},
                    error);
                if (!response)
                {
                    return failure(error);
                }
                return success(listResult(*response,
                                          page,
                                          false,
                                          [](const UnloadedModuleDescriptor& module)
                                          {
                                              return Json {
                                                  {        "va_base",       hexString(module.vaBase)},
                                                  {     "image_size",               module.imageSize},
                                                  {       "is_wow64",                 module.isWow64},
                                                  {       "checksum",      hexSmall(module.checksum)},
                                                  {"time_date_stamp", hexSmall(module.timeDateStamp)},
                                                  {    "unload_time",   hexString(module.unloadTime)},
                                                  {           "text",                    module.text}
                                              };
                                          }));
            }

            if (name == "list_services")
            {
                const auto page = pagination(args);
                const auto response =
                    listServices(socket, {requireString(args, "target"), optionalString(args, "filter")}, error);
                if (!response)
                {
                    return failure(error);
                }
                return success(listResult(response->services,
                                          page,
                                          response->truncated,
                                          [](const ServiceDescriptor& service)
                                          {
                                              return Json {
                                                  {            "object_address",   hexString(service.objectAddress)},
                                                  {                   "ordinal",                    service.ordinal},
                                                  {                "start_type",                  service.startType},
                                                  {              "service_type",                service.serviceType},
                                                  {                     "state",                      service.state},
                                                  {         "controls_accepted", hexSmall(service.controlsAccepted)},
                                                  {           "win32_exit_code",              service.win32ExitCode},
                                                  {"service_specific_exit_code",    service.serviceSpecificExitCode},
                                                  {               "check_point",                 service.checkPoint},
                                                  {                 "wait_hint",                   service.waitHint},
                                                  {                       "pid",                        service.pid},
                                                  {                      "name",                       service.name},
                                                  {              "display_name",                service.displayName},
                                                  {                "image_path",                  service.imagePath},
                                                  {                      "path",                       service.path},
                                                  {                 "user_type",                   service.userType},
                                                  {                   "account",                    service.account}
                                              };
                                          }));
            }

            if (name == "list_users")
            {
                const auto page = pagination(args);
                const auto response =
                    listUsers(socket, {requireString(args, "target"), optionalString(args, "filter")}, error);
                if (!response)
                {
                    return failure(error);
                }
                return success(
                    listResult(response->users,
                               page,
                               response->truncated,
                               [](const UserDescriptor& user)
                               {
                                   return Json {
                                       {    "name",               user.name},
                                       {"reg_hive", hexString(user.regHive)},
                                       {     "sid",                user.sid}
                                   };
                               }));
            }

            if (name == "list_network")
            {
                const auto page = pagination(args);
                const auto response =
                    listNetwork(socket,
                                {requireString(args, "target"), pidValue(args, false), optionalString(args, "filter")},
                                error);
                if (!response)
                {
                    return failure(error);
                }
                return success(
                    listResult(response->entries,
                               page,
                               response->truncated,
                               [](const NetworkEntryDescriptor& entry)
                               {
                                   return Json {
                                       {           "pid",                                                          entry.pid},
                                       {       "process",                                                      entry.process},
                                       {         "state",                                                        entry.state},
                                       {"address_family",                                                entry.addressFamily},
                                       {         "local",   {{"valid", entry.local.valid}, {"address", entry.local.address}}},
                                       {        "remote", {{"valid", entry.remote.valid}, {"address", entry.remote.address}}},
                                       {"object_address",                                     hexString(entry.objectAddress)},
                                       {  "created_time",                                       hexString(entry.createdTime)},
                                       {      "pool_tag",                                         poolTagText(entry.poolTag)}
                                   };
                               }));
            }

            if (name == "list_heaps")
            {
                const auto page = pagination(args);
                const auto response =
                    listHeaps(socket, {requireString(args, "target"), u32Value(args, "pid", true)}, error);
                if (!response)
                {
                    return failure(error);
                }
                return success(listResult(response->heaps,
                                          page,
                                          response->truncated,
                                          [](const HeapDescriptor& heap)
                                          {
                                              return Json {
                                                  {  "address", hexString(heap.address)},
                                                  {    "index",              heap.index},
                                                  {   "number",             heap.number},
                                                  {     "type",               heap.type},
                                                  {"is_32_bit",            heap.is32Bit}
                                              };
                                          }));
            }

            if (name == "list_heap_allocations")
            {
                const auto page     = pagination(args);
                const auto response = listHeapAllocations(
                    socket,
                    {requireString(args, "target"), u32Value(args, "pid", true), numberValue(args, "heap", true)},
                    error);
                if (!response)
                {
                    return failure(error);
                }
                return success(listResult(response->allocations,
                                          page,
                                          response->truncated,
                                          [](const HeapAllocDescriptor& allocation)
                                          {
                                              return Json {
                                                  {"address", hexString(allocation.address)},
                                                  {   "size",               allocation.size},
                                                  {   "type",               allocation.type}
                                              };
                                          }));
            }

            if (name == "list_kernel_drivers")
            {
                const auto page = pagination(args);
                const auto response =
                    listKernelDrivers(socket, {requireString(args, "target"), optionalString(args, "filter")}, error);
                if (!response)
                {
                    return failure(error);
                }
                return success(listResult(response->drivers,
                                          page,
                                          response->truncated,
                                          [](const KernelDriverDescriptor& driver)
                                          {
                                              Json functions = Json::array();
                                              for (const auto function : driver.majorFunctions)
                                              {
                                                  functions.push_back(hexString(function));
                                              }
                                              return Json {
                                                  {              "va",             hexString(driver.va)},
                                                  { "va_driver_start",  hexString(driver.vaDriverStart)},
                                                  {     "driver_size",   hexString(driver.cbDriverSize)},
                                                  {"va_device_object", hexString(driver.vaDeviceObject)},
                                                  { "major_functions",             std::move(functions)},
                                                  {            "name",                      driver.name},
                                                  {            "path",                      driver.path},
                                                  {"service_key_name",            driver.serviceKeyName}
                                              };
                                          }));
            }

            if (name == "list_kernel_devices")
            {
                const auto page = pagination(args);
                const auto response =
                    listKernelDevices(socket, {requireString(args, "target"), optionalString(args, "filter")}, error);
                if (!response)
                {
                    return failure(error);
                }
                return success(listResult(response->devices,
                                          page,
                                          response->truncated,
                                          [](const KernelDeviceDescriptor& device)
                                          {
                                              return Json {
                                                  {                   "va",                 hexString(device.va)},
                                                  {                "depth",                         device.depth},
                                                  {          "device_type",          hexSmall(device.deviceType)},
                                                  {     "va_driver_object",     hexString(device.vaDriverObject)},
                                                  {   "va_attached_device",   hexString(device.vaAttachedDevice)},
                                                  {"va_file_system_device", hexString(device.vaFileSystemDevice)},
                                                  {     "device_type_name",                device.deviceTypeName},
                                                  {          "volume_info",                    device.volumeInfo}
                                              };
                                          }));
            }

            if (name == "list_kernel_objects")
            {
                const auto page = pagination(args);
                const auto response =
                    listKernelObjects(socket, {requireString(args, "target"), optionalString(args, "filter")}, error);
                if (!response)
                {
                    return failure(error);
                }
                return success(listResult(response->objects,
                                          page,
                                          response->truncated,
                                          [](const KernelObjectDescriptor& object)
                                          {
                                              Json children = Json::array();
                                              for (const auto child : object.children)
                                              {
                                                  children.push_back(hexString(child));
                                              }
                                              return Json {
                                                  {                "va",         hexString(object.va)},
                                                  {         "va_parent",   hexString(object.vaParent)},
                                                  {       "child_count", hexString(object.childCount)},
                                                  {          "children",          std::move(children)},
                                                  {"children_truncated",     object.childrenTruncated},
                                                  {              "name",                  object.name},
                                                  {              "type",                  object.type}
                                              };
                                          }));
            }

            if (name == "list_pte")
            {
                const auto page     = pagination(args);
                const auto response = listPte(socket,
                                              {requireString(args, "target"),
                                               u32Value(args, "pid", true),
                                               optionalBool(args, "identify_modules"),
                                               optionalString(args, "filter"),
                                               hexValue(args, "min_address", false),
                                               hexValue(args, "max_address", false)},
                                              error);
                if (!response)
                {
                    return failure(error);
                }
                auto result           = listResult(response->regions,
                                                   page,
                                                   response->truncated,
                                                   [](const PteDescriptor& entry)
                                                   {
                                             return Json {
                                                 {       "va_base",    hexString(entry.vaBase)},
                                                 {         "pages",      hexSmall(entry.pages)},
                                                 {    "page_flags", hexString(entry.pageFlags)},
                                                 {"software_pages",        entry.softwarePages},
                                                 {         "wow64",                entry.wow64},
                                                 {          "text",                 entry.text}
                                             };
                                                   });
                result["map_entries"] = hexString(response->mapEntries);
                return success(std::move(result));
            }

            if (name == "list_pool")
            {
                const auto page     = pagination(args);
                const auto response = listPool(socket,
                                               {requireString(args, "target"),
                                                optionalBool(args, "big"),
                                                optionalString(args, "tag"),
                                                u32Value(args, "min_bytes", false),
                                                hexValue(args, "min_address", false),
                                                hexValue(args, "max_address", false)},
                                               error);
                if (!response)
                {
                    return failure(error);
                }
                auto result           = listResult(response->entries,
                                                   page,
                                                   response->truncated,
                                                   [](const PoolDescriptor& entry)
                                                   {
                                             return Json {
                                                 {        "va",    hexString(entry.va)},
                                                 {     "bytes",            entry.bytes},
                                                 {       "tag", poolTagText(entry.tag)},
                                                 {   "tag_hex",    hexSmall(entry.tag)},
                                                 {     "alloc",       entry.alloc != 0},
                                                 { "pool_type",         entry.poolType},
                                                 {"subsegment",       entry.subsegment}
                                             };
                                                   });
                result["map_entries"] = hexString(response->mapEntries);
                result["map_bytes"]   = hexString(response->mapBytes);
                return success(std::move(result));
            }

            if (name == "list_pool_tags")
            {
                const auto page     = pagination(args);
                const auto response = listPoolTags(
                    socket,
                    {requireString(args, "target"), optionalBool(args, "big"), optionalString(args, "tag")},
                    error);
                if (!response)
                {
                    return failure(error);
                }
                auto result = listResult(
                    response->tags,
                    page,
                    response->truncated,
                    [](const PoolTagDescriptor& tag)
                    {
                        return Json {
                            {    "tag", poolTagText(tag.tag)},
                            {"tag_hex",    hexSmall(tag.tag)},
                            {"entries",          tag.entries}
                        };
                    });
                result["map_tags"]    = hexString(response->mapTags);
                result["map_entries"] = hexString(response->mapEntries);
                return success(std::move(result));
            }

            if (name == "list_pfn")
            {
                const auto page = pagination(args);
                const auto response =
                    listPfn(socket,
                            {requireString(args, "target"),
                             u32Value(args, "pfn_start", true),
                             u32Value(args, "pfn_count", false, 1, static_cast<std::uint32_t>(kMaxPfnEntries))},
                            error);
                if (!response)
                {
                    return failure(error);
                }
                return success(listResult(response->entries,
                                          page,
                                          false,
                                          [](const PfnDescriptor& entry)
                                          {
                                              return Json {
                                                  {            "pfn",                    entry.pfn},
                                                  {  "type_extended",           entry.typeExtended},
                                                  {  "page_location",           entry.pageLocation},
                                                  {            "pid",                    entry.pid},
                                                  {             "va",          hexString(entry.va)},
                                                  {         "va_pte",       hexString(entry.vaPte)},
                                                  {   "original_pte", hexString(entry.originalPte)},
                                                  {      "pte_frame",               entry.pteFrame},
                                                  {"reference_count",         entry.referenceCount},
                                                  {"cache_attribute",         entry.cacheAttribute},
                                                  {       "priority",               entry.priority},
                                                  {          "flags",        hexSmall(entry.flags)},
                                                  {     "page_color",              entry.pageColor}
                                              };
                                          }));
            }

            if (name == "list_vfs")
            {
                const auto page     = pagination(args);
                const auto response = listVfs(
                    socket,
                    {requireString(args, "target"), optionalString(args, "path", "\\"), optionalString(args, "name")},
                    error);
                if (!response)
                {
                    return failure(error);
                }
                return success(listResult(*response,
                                          page,
                                          false,
                                          [](const VfsEntry& entry)
                                          {
                                              return Json {
                                                  {            "name",                      entry.name},
                                                  {            "size",           hexString(entry.size)},
                                                  {   "creation_time",   hexString(entry.creationTime)},
                                                  {"last_access_time", hexString(entry.lastAccessTime)},
                                                  { "last_write_time",  hexString(entry.lastWriteTime)},
                                                  {    "is_directory",               entry.isDirectory}
                                              };
                                          }));
            }

            if (name == "read_vfs")
            {
                const auto offset = hexValue(args, "offset", false);
                const auto size   = readSize(args, kDefaultVfsReadSize);
                const auto response =
                    readVfs(socket, {requireString(args, "target"), requireString(args, "path"), offset, size}, error);
                if (!response)
                {
                    return failure(error);
                }
                return success(Json {
                    {"path", requireString(args, "path")},
                    {"offset", hexString(offset)},
                    {"size", size},
                    {"bytes", bytesJson(*response)}
                });
            }

            if (name == "read_memory")
            {
                const auto address  = hexValue(args, "address", true);
                const auto size     = readSize(args, kDefaultReadSize);
                const auto pid      = u32Value(args, "pid", true);
                const auto response = readMemory(socket, {requireString(args, "target"), pid, address, size}, error);
                if (!response)
                {
                    return failure(error);
                }
                return success(Json {
                    {    "pid",                  pid},
                    {"address",   hexString(address)},
                    {   "size",                 size},
                    {  "bytes", bytesJson(*response)}
                });
            }

            if (name == "scatter_read")
            {
                const auto pid    = u32Value(args, "pid", true);
                const auto ranges = rangeArg(args, "ranges", true);
                const auto repeat = u32Value(args, "repeat", false, 1, 1024);
                const bool clear  = optionalBool(args, "clear");

                auto session = createScatter(socket, requireString(args, "target"), pid, error);
                if (!session)
                {
                    return failure(error);
                }

                std::vector<ScatterItem> entries;
                entries.reserve(ranges.size());
                for (const auto& range : ranges)
                {
                    entries.push_back({range.address, static_cast<std::uint32_t>(range.size), false, {}});
                }
                if (!session->prepare(entries, error))
                {
                    return failure(error);
                }

                Json rounds = Json::array();
                for (std::uint32_t round = 0; round < repeat; ++round)
                {
                    auto results = round == 0 ? session->execute(error) : session->executeRead(error);
                    if (!results)
                    {
                        return failure(error);
                    }
                    Json items = Json::array();
                    for (const auto& result : *results)
                    {
                        items.push_back(Json {
                            {       "address",                          hexString(result.address)},
                            {"requested_size",                               result.requestedSize},
                            {    "bytes_read",                                   result.bytesRead},
                            {            "ok",                                          result.ok},
                            {         "bytes", result.ok ? bytesJson(result.data) : Json(nullptr)}
                        });
                    }
                    rounds.push_back(std::move(items));
                }

                if (clear && !session->clear(error))
                {
                    return failure(error);
                }
                if (!session->close(error))
                {
                    return failure(error);
                }
                return success(Json {
                    {   "pid",               pid},
                    {"rounds", std::move(rounds)}
                });
            }

            if (name == "list_vms")
            {
                const auto page     = pagination(args);
                const auto response = listVms(socket, requireString(args, "target"), error);
                if (!response)
                {
                    return failure(error);
                }
                std::size_t index  = page.offset;
                auto        result = listResult(*response,
                                                page,
                                                false,
                                                [&index](const VmDescriptor& vm)
                                                {
                                             return Json {
                                                 {        "index",              index++},
                                                 {         "name",              vm.name},
                                                 {      "gpa_max", hexString(vm.gpaMax)},
                                                 {         "kind",              vm.kind},
                                                 {  "system_kind",        vm.systemKind},
                                                 { "partition_id",       vm.partitionId},
                                                 {"version_build",      vm.versionBuild},
                                                 {   "vm_mem_pid",          vm.vmMemPid},
                                                 {       "active",            vm.active},
                                                 {    "read_only",          vm.readOnly},
                                                 {"physical_only",      vm.physicalOnly}
                                             };
                                                });
                return success(std::move(result));
            }

            if (name == "read_physical_memory")
            {
                const auto vmIndex = u32Value(args, "vm_index", false);
                const auto address = hexValue(args, "address", true);
                const auto size    = readSize(args, kDefaultReadSize);
                const auto response =
                    readPhysicalMemory(socket, {requireString(args, "target"), vmIndex, address, size}, error);
                if (!response)
                {
                    return failure(error);
                }
                return success(Json {
                    {"vm_index",              vmIndex},
                    { "address",   hexString(address)},
                    {    "size",                 size},
                    {   "bytes", bytesJson(*response)}
                });
            }

            if (name == "translate_gpa")
            {
                const auto response = translateGpa(
                    socket,
                    {requireString(args, "target"), u32Value(args, "vm_index", false), hexValue(args, "address", true)},
                    error);
                if (!response)
                {
                    return failure(error);
                }
                return success(Json {
                    {        "pa", hexString(response->pa)},
                    {        "va", hexString(response->va)},
                    {"pa_present",     response->paPresent},
                    {"va_present",     response->vaPresent}
                });
            }

            if (name == "virt2phys")
            {
                const auto response = virt2Phys(
                    socket,
                    {requireString(args, "target"), u32Value(args, "pid", true), hexValue(args, "address", true)},
                    error);
                if (!response)
                {
                    return failure(error);
                }
                return success(Json {
                    {"pa", hexString(*response)}
                });
            }

            if (name == "physical_scatter")
            {
                const auto vmIndex = u32Value(args, "vm_index", false);
                const auto ranges  = rangeArg(args, "ranges", true);

                std::vector<ScatterItem> entries;
                entries.reserve(ranges.size());
                for (const auto& range : ranges)
                {
                    entries.push_back({range.address, static_cast<std::uint32_t>(range.size), false, {}});
                }
                const auto response =
                    physicalScatter(socket, {requireString(args, "target"), vmIndex, std::move(entries)}, error);
                if (!response)
                {
                    return failure(error);
                }
                Json items = Json::array();
                for (const auto& result : *response)
                {
                    items.push_back(Json {
                        {       "address",                          hexString(result.address)},
                        {"requested_size",                               result.requestedSize},
                        {    "bytes_read",                                   result.bytesRead},
                        {            "ok",                                          result.ok},
                        {         "bytes", result.ok ? bytesJson(result.data) : Json(nullptr)}
                    });
                }
                return success(Json {
                    {"vm_index",          vmIndex},
                    {   "items", std::move(items)}
                });
            }

            if (name == "query_virtual")
            {
                const auto response = queryVirtual(
                    socket,
                    {requireString(args, "target"), u32Value(args, "pid", true), hexValue(args, "address", true)},
                    error);
                if (!response)
                {
                    return failure(error);
                }
                return success(Json {
                    {      "base_address",      hexString(response->baseAddress)},
                    {   "allocation_base",   hexString(response->allocationBase)},
                    {       "region_size",       hexString(response->regionSize)},
                    {"allocation_protect", hexSmall(response->allocationProtect)},
                    {             "state",             hexSmall(response->state)},
                    {           "protect",           hexSmall(response->protect)},
                    {              "type",              hexSmall(response->type)},
                    {      "partition_id",                 response->partitionId}
                });
            }

            if (name == "read_pages")
            {
                const auto pid      = pidValue(args, true);
                const auto ranges   = rangeArg(args, "ranges", true);
                const auto response = readPages(socket, {requireString(args, "target"), pid, ranges}, error);
                if (!response)
                {
                    return failure(error);
                }
                Json pages = Json::array();
                for (const auto& page : *response)
                {
                    pages.push_back(Json {
                        {"address",                        hexString(page.address)},
                        {     "ok",                                        page.ok},
                        {  "bytes", page.ok ? bytesJson(page.data) : Json(nullptr)}
                    });
                }
                return success(Json {
                    {  "pid",              pid},
                    {"pages", std::move(pages)}
                });
            }

            if (name == "prefetch_pages")
            {
                const auto pid      = pidValue(args, true);
                const auto ranges   = rangeArg(args, "ranges", true);
                const auto response = prefetchPages(socket, {requireString(args, "target"), pid, ranges}, error);
                if (!response)
                {
                    return failure(error);
                }
                return success(Json {
                    {  "pid",       pid},
                    {"pages", *response}
                });
            }

            if (name == "search_memory")
            {
                const bool hasText = hasArg(args, "pattern");
                const bool hasHex  = hasArg(args, "hex_pattern");
                if (hasText == hasHex)
                {
                    throw ArgumentError("search_memory needs exactly one of 'pattern' and 'hex_pattern'");
                }

                SearchTerm term;
                if (hasHex)
                {
                    const auto parsed = parseHexPattern(optionalString(args, "hex_pattern"));
                    if (!parsed)
                    {
                        throw ArgumentError("argument 'hex_pattern' needs 1 to 32 bytes, with ?? for wildcards");
                    }
                    term = *parsed;
                }
                else
                {
                    term = makeSearchTerm(optionalString(args, "pattern"));
                }
                if (term.pattern.size() > kMaxSearchPatternLength)
                {
                    throw ArgumentError("a search pattern is at most " + std::to_string(kMaxSearchPatternLength)
                                        + " bytes");
                }
                if (!isSearchableTerm(term))
                {
                    throw ArgumentError("a search pattern needs at least one non-zero byte");
                }

                const auto page  = pagination(args);
                const auto align = u32Value(args, "align", false, 1, kMaxSearchAlignBytes);
                if (align == 0 || (align & (align - 1)) != 0)
                {
                    throw ArgumentError("argument 'align' must be a power of two");
                }

                const auto response =
                    searchMemory(socket,
                                 {requireString(args, "target"),
                                  pidValue(args, true),
                                  std::move(term),
                                  align,
                                  hexValue(args, "min_address", false),
                                  hexValue(args, "max_address", false),
                                  u32Value(args, "max_results", false, kDefaultSearchResults, kMaxSearchHits)},
                                 error);
                if (!response)
                {
                    return failure(error);
                }
                auto result = listResult(response->addresses,
                                         page,
                                         response->truncated,
                                         [](std::uint64_t address)
                                         {
                                             return hexString(address);
                                         });
                return success(std::move(result));
            }

            if (name == "config_set")
            {
                const auto optionText = requireString(args, "option");
                const auto option     = parseConfigOption(optionText);
                if (!option)
                {
                    throw ArgumentError("unknown config option '" + optionText + "'");
                }
                const auto pid      = pidValue(args, false);
                const bool hasValue = hasArg(args, "value");
                if (configOptionUsesPid(*option) && pid == 0)
                {
                    throw ArgumentError("config_set needs 'pid' for this option");
                }
                if (!configOptionUsesPid(*option) && pid != 0)
                {
                    throw ArgumentError("config_set does not take 'pid' for this option");
                }
                if (configOptionRequiresValue(*option) && !hasValue)
                {
                    throw ArgumentError("config_set needs 'value' for this option");
                }
                const auto value    = numberValue(args, "value", false);
                const auto response = setConfig(socket, {requireString(args, "target"), *option, pid, value}, error);
                if (!response)
                {
                    return failure(error);
                }
                return success(Json {
                    {"option", configOptionName(*option)},
                    { "value",      hexString(*response)}
                });
            }

            if (name == "write_memory")
            {
                const auto response = writeMemory(socket,
                                                  {requireString(args, "target"),
                                                   u32Value(args, "pid", true),
                                                   hexValue(args, "address", true),
                                                   dataArg(args)},
                                                  error);
                if (!response)
                {
                    return failure(error);
                }
                return success(Json {
                    {"bytes_written", hexString(*response)}
                });
            }

            if (name == "write_vfs")
            {
                const auto response = writeVfs(socket,
                                               {requireString(args, "target"),
                                                requireString(args, "path"),
                                                hexValue(args, "offset", false),
                                                dataArg(args)},
                                               error);
                if (!response)
                {
                    return failure(error);
                }
                return success(Json {
                    {"bytes_written", hexString(*response)}
                });
            }

            if (name == "write_physical_memory")
            {
                const auto response = writePhysicalMemory(socket,
                                                          {requireString(args, "target"),
                                                           u32Value(args, "vm_index", false),
                                                           hexValue(args, "address", true),
                                                           dataArg(args)},
                                                          error);
                if (!response)
                {
                    return failure(error);
                }
                return success(Json {
                    {"bytes_written", hexString(*response)}
                });
            }

            return failure("tool '" + std::string {name} + "' is not implemented");
        }
        catch (const ArgumentError& argumentError)
        {
            return failure(argumentError.what());
        }
        catch (const Json::exception& jsonError)
        {
            return failure("invalid JSON arguments: " + std::string {jsonError.what()});
        }
        catch (const std::exception& exception)
        {
            return failure(exception.what());
        }
    }
} // namespace mpfshost::mcp
