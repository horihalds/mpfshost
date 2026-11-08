#ifndef MPFSHOST_CORE_DEBUG_DEBUG_INSPECTOR_HPP
#define MPFSHOST_CORE_DEBUG_DEBUG_INSPECTOR_HPP

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mpfshost::debug
{
    struct DebugProcessDtbs
    {
        std::uint64_t dtb {};
        std::uint64_t dtbUser {};
    };

    struct DebugModuleHit
    {
        std::uint64_t base {};
        std::uint64_t imageSize {};
        std::uint64_t offset {};
        std::string   name;
    };

    struct DebugExportHit
    {
        bool          found {};
        std::uint64_t address {};
        std::uint64_t offset {};
        std::string   name;
    };

    struct DebugPdbHit
    {
        std::string   symbol;
        std::uint32_t displacement {};
    };

    // Process context and symbolization behind a watchpoint. Implemented by the privileged host handle so the
    // controller stays independent of MemProcFS. A failed lookup is reported by an empty optional, not an error;
    // only an unusable handle or a transport failure sets `error`.
    class DebugInspector
    {
    public:
        virtual ~DebugInspector() = default;

        [[nodiscard]] virtual std::optional<DebugProcessDtbs> processDtbs(std::uint32_t pid, std::string& error) = 0;
        [[nodiscard]] virtual std::optional<DebugModuleHit>
        moduleForAddress(std::uint32_t pid, std::uint64_t address, std::string& error) = 0;
        [[nodiscard]] virtual DebugExportHit
        nearestExport(std::uint32_t pid, const std::string& module, std::uint64_t address, std::string& error) = 0;
        [[nodiscard]] virtual std::optional<DebugPdbHit>
        pdbSymbol(std::uint32_t pid, const std::string& module, std::uint64_t offset, std::string& error) = 0;
        [[nodiscard]] virtual bool readMemory(std::uint32_t           pid,
                                              std::uint64_t           address,
                                              std::uint32_t           size,
                                              std::vector<std::byte>& out,
                                              std::string&            error)                              = 0;
    };
} // namespace mpfshost::debug

#endif // MPFSHOST_CORE_DEBUG_DEBUG_INSPECTOR_HPP
