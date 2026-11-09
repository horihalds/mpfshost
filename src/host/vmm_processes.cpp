#include "host/vmm_processes.hpp"

#include "host/vmm_text.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

namespace mpfshost
{
    namespace
    {
        std::string processName(const VMMDLL_PROCESS_INFORMATION& info)
        {
            if (info.szNameLong[0] != '\0')
            {
                return info.szNameLong;
            }
            return info.szName;
        }

        // VMMDLL_ProcessGetInformationString hands back an allocation. Copy it under the cap and free it in the same
        // expression-free scope so no early return can leak it; a null answer means "MemProcFS has no value".
        std::string processOptionString(VMM_HANDLE handle, DWORD pid, DWORD option)
        {
            LPSTR text = VMMDLL_ProcessGetInformationString(handle, pid, option);
            if (text == nullptr)
            {
                return {};
            }
            std::string value = copyMapText(text);
            VMMDLL_MemFree(text);
            return value;
        }

        std::string copyFixedText(const CHAR* text, std::size_t capacity)
        {
            const std::size_t length = std::min<std::size_t>(std::strlen(text), capacity);
            return std::string {text, length};
        }
    } // namespace

    std::optional<std::vector<ProcessDescriptor>> listVmmProcesses(VMM_HANDLE handle, std::string& error)
    {
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        SIZE_T count = 0;
        if (!VMMDLL_PidList(handle, nullptr, &count))
        {
            error = "VMMDLL_PidList failed to report the process count";
            return std::nullopt;
        }

        std::vector<DWORD> pids(count);
        if (count != 0 && !VMMDLL_PidList(handle, pids.data(), &count))
        {
            error = "VMMDLL_PidList failed to return the process ids";
            return std::nullopt;
        }
        // The second call may shrink *count, so trust the returned value over the allocation size.
        pids.resize(count);

        std::vector<ProcessDescriptor> processes;
        processes.reserve(pids.size());
        for (const DWORD pid : pids)
        {
            VMMDLL_PROCESS_INFORMATION info {};
            info.magic    = VMMDLL_PROCESS_INFORMATION_MAGIC;
            info.wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;
            auto size     = sizeof(VMMDLL_PROCESS_INFORMATION);
            if (!VMMDLL_ProcessGetInformation(handle, pid, &info, &size))
            {
                continue;
            }

            ProcessDescriptor descriptor;
            descriptor.pid       = info.dwPID;
            descriptor.parentPid = info.dwPPID;
            descriptor.wow64     = info.win.fWow64 != 0;
            descriptor.name      = processName(info);
            descriptor.imagePath =
                processOptionString(handle, pid, VMMDLL_PROCESS_INFORMATION_OPT_STRING_PATH_USER_IMAGE);
            processes.push_back(std::move(descriptor));
        }
        return processes;
    }

    std::optional<std::vector<ProcessDtbRecord>> listVmmProcessDtbs(VMM_HANDLE handle, std::string& error)
    {
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        SIZE_T count = 0;
        if (!VMMDLL_PidList(handle, nullptr, &count))
        {
            error = "VMMDLL_PidList failed to report the process count";
            return std::nullopt;
        }

        std::vector<DWORD> pids(count);
        if (count != 0 && !VMMDLL_PidList(handle, pids.data(), &count))
        {
            error = "VMMDLL_PidList failed to return the process ids";
            return std::nullopt;
        }
        pids.resize(count);

        // PidList names live processes only; GetInformationAll would also include terminated ones whose stale
        // DTB must not answer a CR3 lookup.
        std::vector<ProcessDtbRecord> records;
        records.reserve(pids.size());
        for (const DWORD pid : pids)
        {
            VMMDLL_PROCESS_INFORMATION info {};
            info.magic    = VMMDLL_PROCESS_INFORMATION_MAGIC;
            info.wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;
            auto size     = sizeof(VMMDLL_PROCESS_INFORMATION);
            if (!VMMDLL_ProcessGetInformation(handle, pid, &info, &size) || info.paDTB == 0)
            {
                continue;
            }

            ProcessDtbRecord record;
            record.process.pid       = info.dwPID;
            record.process.parentPid = info.dwPPID;
            record.process.wow64     = info.win.fWow64 != 0;
            record.process.name      = processName(info);
            record.dtb               = info.paDTB;
            record.dtbUser           = info.paDTB_UserOpt;
            records.push_back(std::move(record));
        }
        return records;
    }

    std::optional<std::vector<ProcessSummary>> listAllVmmProcesses(VMM_HANDLE handle, std::string& error)
    {
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        PVMMDLL_PROCESS_INFORMATION infos = nullptr;
        DWORD                       count = 0;
        if (!VMMDLL_ProcessGetInformationAll(handle, &infos, &count))
        {
            error = "VMMDLL_ProcessGetInformationAll failed";
            return std::nullopt;
        }

        struct InformationGuard
        {
            PVMMDLL_PROCESS_INFORMATION infos;

            ~InformationGuard()
            {
                VMMDLL_MemFree(infos);
            }
        } guard {infos};

        std::vector<ProcessSummary> processes;
        processes.reserve(count);
        for (DWORD i = 0; i < count; ++i)
        {
            const auto& info = infos[i];

            ProcessSummary summary;
            summary.process.pid       = info.dwPID;
            summary.process.parentPid = info.dwPPID;
            summary.process.wow64     = info.win.fWow64 != 0;
            summary.process.name      = processName(info);
            summary.process.imagePath =
                processOptionString(handle, info.dwPID, VMMDLL_PROCESS_INFORMATION_OPT_STRING_PATH_USER_IMAGE);
            summary.state = info.dwState;
            processes.push_back(std::move(summary));
        }
        return processes;
    }

    std::optional<ProcessInfoResponse>
    queryVmmProcess(VMM_HANDLE handle, const ProcessInfoRequest& request, LookupStatus& status, std::string& error)
    {
        status = LookupStatus::Failed;
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        DWORD pid = request.pid;
        if (pid == 0)
        {
            if (request.name.empty())
            {
                error = "a process info request needs either a pid or a name";
                return std::nullopt;
            }
            if (!VMMDLL_PidGetFromName(handle, request.name.c_str(), &pid) || pid == 0)
            {
                status = LookupStatus::NotFound;
                error  = "no process named '" + request.name + "'";
                return std::nullopt;
            }
        }

        VMMDLL_PROCESS_INFORMATION info {};
        info.magic    = VMMDLL_PROCESS_INFORMATION_MAGIC;
        info.wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;
        auto size     = sizeof(VMMDLL_PROCESS_INFORMATION);
        if (!VMMDLL_ProcessGetInformation(handle, pid, &info, &size))
        {
            status = LookupStatus::NotFound;
            error  = "process " + std::to_string(pid) + " is not present";
            return std::nullopt;
        }

        ProcessInfoResponse response;
        response.process.pid       = info.dwPID;
        response.process.parentPid = info.dwPPID;
        response.process.wow64     = info.win.fWow64 != 0;
        response.process.name      = processName(info);
        response.state             = info.dwState;
        response.memoryModel       = static_cast<std::uint16_t>(info.tpMemoryModel);
        response.systemType        = static_cast<std::uint16_t>(info.tpSystem);
        response.userOnly          = info.fUserOnly != 0;
        response.dtb               = info.paDTB;
        response.dtbUser           = info.paDTB_UserOpt;
        response.eprocessVa        = info.win.vaEPROCESS;
        response.pebVa             = info.win.vaPEB;
        response.peb32Va           = info.win.vaPEB32;
        response.sessionId         = info.win.dwSessionId;
        response.luid              = info.win.qwLUID;
        response.integrityLevel    = static_cast<std::uint32_t>(info.win.IntegrityLevel);
        response.sid               = copyFixedText(info.win.szSID, sizeof(info.win.szSID) - 1);
        response.imagePath  = processOptionString(handle, pid, VMMDLL_PROCESS_INFORMATION_OPT_STRING_PATH_USER_IMAGE);
        response.kernelPath = processOptionString(handle, pid, VMMDLL_PROCESS_INFORMATION_OPT_STRING_PATH_KERNEL);
        response.cmdline    = processOptionString(handle, pid, VMMDLL_PROCESS_INFORMATION_OPT_STRING_CMDLINE);

        status = LookupStatus::Ok;
        return response;
    }
} // namespace mpfshost
