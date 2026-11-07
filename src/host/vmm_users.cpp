#include "host/vmm_users.hpp"

#include "core/ipc/frame.hpp"
#include "host/vmm_text.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace mpfshost
{
    std::optional<UserListResult>
    listVmmUsers(VMM_HANDLE handle, const UserListRequest& request, LookupStatus& status, std::string& error)
    {
        (void)status;
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        // The user map is whole-machine with no pid to probe, so a failing call cannot be told apart from an empty
        // one; a target whose system maps stay unparsed therefore reads as Internal, not as no accounts.
        PVMMDLL_MAP_USER                                                  map = nullptr;
        const BOOL                                                        ok  = VMMDLL_Map_GetUsersU(handle, &map);
        const std::unique_ptr<VMMDLL_MAP_USER, decltype(&VMMDLL_MemFree)> guard {map, &VMMDLL_MemFree};
        if (!ok || map == nullptr)
        {
            error = "VMMDLL_Map_GetUsersU failed";
            return std::nullopt;
        }

        UserListResult result;
        result.users.reserve(std::min<std::size_t>(map->cMap, kMaxUserEntries));
        for (DWORD i = 0; i < map->cMap; ++i)
        {
            const auto&    entry = map->pMap[i];
            UserDescriptor user;
            user.name    = copyMapText(entry.uszText);
            user.regHive = entry.vaRegHive;
            user.sid     = copyMapText(entry.uszSID);

            if (!matchesUserFilter(request, user))
            {
                continue;
            }
            if (result.users.size() == kMaxUserEntries)
            {
                result.truncated = true;
                break;
            }
            result.users.push_back(std::move(user));
        }
        return result;
    }
} // namespace mpfshost
