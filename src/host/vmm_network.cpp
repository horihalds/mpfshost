#include "host/vmm_network.hpp"

#include "core/ipc/frame.hpp"
#include "host/vmm_text.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace mpfshost
{
    namespace
    {
        std::string processText(const VMMDLL_PROCESS_INFORMATION& info)
        {
            if (info.szNameLong[0] != '\0')
            {
                return info.szNameLong;
            }
            return info.szName;
        }

        // A connection only carries the owning pid, so the name is looked up once per pid and reused across the
        // whole answer; a pid MemProcFS cannot resolve stays empty rather than failing the request.
        std::string lookupProcessName(VMM_HANDLE handle, DWORD pid, std::unordered_map<DWORD, std::string>& cache)
        {
            const auto cached = cache.find(pid);
            if (cached != cache.end())
            {
                return cached->second;
            }

            std::string name;
            if (pid != 0)
            {
                VMMDLL_PROCESS_INFORMATION info {};
                info.magic    = VMMDLL_PROCESS_INFORMATION_MAGIC;
                info.wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;
                auto size     = sizeof(VMMDLL_PROCESS_INFORMATION);
                if (VMMDLL_ProcessGetInformation(handle, pid, &info, &size))
                {
                    name = processText(info);
                }
            }
            return cache.emplace(pid, std::move(name)).first->second;
        }

        // MemProcFS renders the address family and the address of each endpoint but keeps the port in its own field,
        // so the port is appended here to give the analyst the same address:port text its own netstat shows.
        std::string endpointText(LPCSTR address, WORD port, bool valid)
        {
            if (!valid)
            {
                return {};
            }
            std::string text = copyMapText(address);
            if (text.empty())
            {
                return text;
            }
            if (text.find(':') != std::string::npos)
            {
                text = "[" + text + "]";
            }
            return text + ":" + std::to_string(port);
        }
    } // namespace

    std::optional<NetworkListResult>
    listVmmNetwork(VMM_HANDLE handle, const NetworkListRequest& request, LookupStatus& status, std::string& error)
    {
        (void)status;
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        // The connection map is whole-machine with no pid argument, so a failing call cannot be told apart from an
        // empty table; the pid scope is therefore applied while copying instead of being passed to MemProcFS.
        PVMMDLL_MAP_NET                                                  map = nullptr;
        const BOOL                                                       ok  = VMMDLL_Map_GetNetU(handle, &map);
        const std::unique_ptr<VMMDLL_MAP_NET, decltype(&VMMDLL_MemFree)> guard {map, &VMMDLL_MemFree};
        if (!ok || map == nullptr)
        {
            error = "VMMDLL_Map_GetNetU failed";
            return std::nullopt;
        }

        std::unordered_map<DWORD, std::string> processNames;
        NetworkListResult                      result;
        result.entries.reserve(std::min<std::size_t>(map->cMap, kMaxNetworkEntries));
        for (DWORD i = 0; i < map->cMap; ++i)
        {
            const auto&            entry = map->pMap[i];
            NetworkEntryDescriptor descriptor;
            descriptor.pid            = entry.dwPID;
            descriptor.process        = lookupProcessName(handle, entry.dwPID, processNames);
            descriptor.state          = entry.dwState;
            descriptor.addressFamily  = entry.AF;
            descriptor.local.valid    = entry.Src.fValid != 0;
            descriptor.local.address  = endpointText(entry.Src.uszText, entry.Src.port, descriptor.local.valid);
            descriptor.remote.valid   = entry.Dst.fValid != 0;
            descriptor.remote.address = endpointText(entry.Dst.uszText, entry.Dst.port, descriptor.remote.valid);
            descriptor.objectAddress  = entry.vaObj;
            descriptor.createdTime    = entry.ftTime;
            descriptor.poolTag        = entry.dwPoolTag;

            if (!matchesNetworkFilter(request, descriptor))
            {
                continue;
            }
            if (result.entries.size() == kMaxNetworkEntries)
            {
                result.truncated = true;
                break;
            }
            result.entries.push_back(std::move(descriptor));
        }
        return result;
    }
} // namespace mpfshost
