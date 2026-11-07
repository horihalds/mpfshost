#include "pch.hpp"

#include "cli/render.hpp"
#include "core/ipc/message.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

namespace mpfshost::cli
{
    const char* moduleKindName(std::uint16_t kind)
    {
        switch (kind)
        {
        case 0:
            return "normal";
        case 1:
            return "data";
        case 2:
            return "notlinked";
        case 3:
            return "injected";
        default:
            return "unknown";
        }
    }

    std::string poolTagText(std::uint32_t tag)
    {
        std::string text(4, '.');
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const auto value = static_cast<unsigned char>((tag >> (8 * i)) & 0xFF);
            text[i]          = value >= 0x20 && value < 0x7F ? static_cast<char>(value) : '.';
        }
        return text;
    }

    const char* poolTypeName(std::uint8_t type)
    {
        switch (type)
        {
        case 0:
            return "unknown";
        case 1:
            return "NonPagedPool";
        case 2:
            return "NonPagedPoolNx";
        case 3:
            return "PagedPool";
        default:
            return "invalid";
        }
    }

    const char* poolSubsegmentName(std::uint8_t subsegment)
    {
        switch (subsegment)
        {
        case 0:
            return "unknown";
        case 1:
            return "na";
        case 2:
            return "big";
        case 3:
            return "large";
        case 4:
            return "vs";
        case 5:
            return "lfh";
        default:
            return "invalid";
        }
    }

    const char* pfnTypeName(std::uint8_t type)
    {
        switch (type)
        {
        case 0:
            return "Zero";
        case 1:
            return "Free";
        case 2:
            return "Standby";
        case 3:
            return "Modified";
        case 4:
            return "ModNoWr";
        case 5:
            return "Bad";
        case 6:
            return "Active";
        case 7:
            return "Transit";
        default:
            return "invalid";
        }
    }

    const char* pfnExtendedTypeName(std::uint8_t type)
    {
        switch (type)
        {
        case 0:
            return "-";
        case 1:
            return "Unused";
        case 2:
            return "ProcPriv";
        case 3:
            return "PageTable";
        case 4:
            return "LargePage";
        case 5:
            return "DriverLock";
        case 6:
            return "Shareable";
        case 7:
            return "File";
        default:
            return "invalid";
        }
    }

    std::string pteFlagNames(std::uint64_t flags)
    {
        std::string names;
        const auto  append = [&names](bool set, const char* name)
        {
            if (!set)
            {
                return;
            }
            if (!names.empty())
            {
                names += ',';
            }
            names += name;
        };
        append((flags & 0x2) != 0, "w");
        append((flags & 0x4) != 0, "ns");
        append((flags & 0x8000000000000000ULL) != 0, "nx");
        return names.empty() ? "-" : names;
    }

    const char* pteTypeName(std::uint32_t type)
    {
        switch (type)
        {
        case 0:
            return "na";
        case 1:
            return "hardware";
        case 2:
            return "transition";
        case 3:
            return "prototype";
        case 4:
            return "demandzero";
        case 5:
            return "compressed";
        case 6:
            return "pagefile";
        case 7:
            return "file";
        default:
            return "invalid";
        }
    }

    std::string vadExFlagNames(std::uint8_t flags)
    {
        std::string names;
        const auto  append = [&names](bool set, const char* name)
        {
            if (!set)
            {
                return;
            }
            if (!names.empty())
            {
                names += ',';
            }
            names += name;
        };
        append((flags & 0x01) != 0, "hardware");
        append((flags & 0x10) != 0, "w");
        append((flags & 0x40) != 0, "k");
        append((flags & 0x80) != 0, "nx");
        return names.empty() ? "-" : names;
    }

    std::string pfnFlagNames(std::uint8_t flags)
    {
        std::string names;
        const auto  append = [&names](bool set, const char* name)
        {
            if (!set)
            {
                return;
            }
            if (!names.empty())
            {
                names += ',';
            }
            names += name;
        };
        append((flags & (1U << 0)) != 0, "wip");
        append((flags & (1U << 1)) != 0, "modified");
        append((flags & (1U << 2)) != 0, "rip");
        append((flags & (1U << 3)) != 0, "remove");
        append((flags & (1U << 4)) != 0, "parity");
        append((flags & (1U << 5)) != 0, "inpageerror");
        append((flags & (1U << 6)) != 0, "prototype");
        append((flags & (1U << 7)) != 0, "kernelstack");
        return names.empty() ? "-" : names;
    }

    std::string hexPointer(std::uint64_t value)
    {
        if (value == 0)
        {
            return "-";
        }
        char buffer[19] {};
        std::snprintf(buffer, sizeof(buffer), "0x%016llx", static_cast<unsigned long long>(value));
        return buffer;
    }

    std::string mapFlagNames(std::uint16_t flags)
    {
        std::string names;
        const auto  append = [&names](bool set, const char* name)
        {
            if (!set)
            {
                return;
            }
            if (!names.empty())
            {
                names += ',';
            }
            names += name;
        };
        append((flags & mpfshost::kMemoryMapFlagImage) != 0, "image");
        append((flags & mpfshost::kMemoryMapFlagFile) != 0, "file");
        append((flags & mpfshost::kMemoryMapFlagPageFile) != 0, "pagefile");
        append((flags & mpfshost::kMemoryMapFlagPrivate) != 0, "private");
        append((flags & mpfshost::kMemoryMapFlagTeb) != 0, "teb");
        append((flags & mpfshost::kMemoryMapFlagStack) != 0, "stack");
        append((flags & mpfshost::kMemoryMapFlagHeap) != 0, "heap");
        return names.empty() ? "-" : names;
    }

    std::string protectName(std::uint32_t protect)
    {
        const char* base {};
        switch (protect & 0xFF)
        {
        case 0x01:
            base = "noaccess";
            break;
        case 0x02:
            base = "readonly";
            break;
        case 0x04:
            base = "readwrite";
            break;
        case 0x08:
            base = "writecopy";
            break;
        case 0x10:
            base = "execute";
            break;
        case 0x20:
            base = "execute-read";
            break;
        case 0x40:
            base = "execute-readwrite";
            break;
        case 0x80:
            base = "execute-writecopy";
            break;
        default:
            base = "none";
            break;
        }

        std::string name {base};
        if ((protect & 0x100) != 0)
        {
            name += "|guard";
        }
        if ((protect & 0x200) != 0)
        {
            name += "|nocache";
        }
        if ((protect & 0x400) != 0)
        {
            name += "|writecombine";
        }
        return name;
    }

    const char* stateName(std::uint32_t state)
    {
        switch (state)
        {
        case 0x1000:
            return "commit";
        case 0x2000:
            return "reserve";
        case 0x4000:
            return "decommit";
        case 0x10000:
            return "free";
        case 0x40000:
            return "reset";
        default:
            return "unknown";
        }
    }

    const char* heapTypeName(std::uint8_t type)
    {
        switch (type)
        {
        case 1:
            return "nt";
        case 2:
            return "segment";
        default:
            return "n/a";
        }
    }

    const char* heapAllocTypeName(std::uint8_t type)
    {
        switch (type)
        {
        case 1:
            return "nt-heap";
        case 2:
            return "nt-lfh";
        case 3:
            return "nt-large";
        case 4:
            return "nt-na";
        case 5:
            return "seg-vs";
        case 6:
            return "seg-lfh";
        case 7:
            return "seg-large";
        case 8:
            return "seg-na";
        default:
            return "n/a";
        }
    }

    const char* regionTypeName(std::uint32_t type)
    {
        switch (type)
        {
        case 0x20000:
            return "private";
        case 0x40000:
            return "mapped";
        case 0x1000000:
            return "image";
        default:
            return "none";
        }
    }
} // namespace mpfshost::cli
