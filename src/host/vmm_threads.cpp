#include "host/vmm_threads.hpp"

#include "core/ipc/frame.hpp"
#include "core/server/request_deadline.hpp"
#include "core/server/thread_provider.hpp"
#include "host/vmm_text.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>

namespace mpfshost
{
    namespace
    {
        ThreadFrame toFrame(const VMMDLL_MAP_THREAD_CALLSTACKENTRY& entry)
        {
            ThreadFrame frame;
            frame.index            = entry.i;
            frame.returnAddress    = entry.vaRetAddr;
            frame.stackPointer     = entry.vaRSP;
            frame.frameBase        = entry.vaBaseSP;
            frame.displacement     = entry.cbDisplacement;
            frame.registersPresent = entry.fRegPresent != 0;
            frame.module           = copyMapText(entry.uszModule);
            frame.function         = copyMapText(entry.uszFunction);
            return frame;
        }

        void appendCallstack(VMM_HANDLE handle, std::uint32_t pid, ThreadDescriptor& thread)
        {
            PVMMDLL_MAP_THREAD_CALLSTACK stack = nullptr;
            const BOOL                   ok    = VMMDLL_Map_GetThread_CallstackU(handle, pid, thread.tid, 0, &stack);
            const std::unique_ptr<VMMDLL_MAP_THREAD_CALLSTACK, decltype(&VMMDLL_MemFree)> guard {stack,
                                                                                                 &VMMDLL_MemFree};
            if (!ok || stack == nullptr)
            {
                thread.callstackStatus = 1;
                return;
            }

            const auto count = std::min<std::size_t>(stack->cMap, kMaxCallstackFrames);
            thread.frames.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                thread.frames.push_back(toFrame(stack->pMap[i]));
            }
        }
    } // namespace

    std::optional<std::vector<ThreadDescriptor>>
    listVmmThreads(VMM_HANDLE handle, const ThreadListRequest& request, std::string& error)
    {
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        PVMMDLL_MAP_THREAD map = nullptr;
        const BOOL         ok  = VMMDLL_Map_GetThread(handle, request.pid, &map);
        const std::unique_ptr<VMMDLL_MAP_THREAD, decltype(&VMMDLL_MemFree)> guard {map, &VMMDLL_MemFree};
        if (!ok || map == nullptr)
        {
            error = "VMMDLL_Map_GetThread failed";
            return std::nullopt;
        }

        std::vector<ThreadDescriptor> threads;
        threads.reserve(map->cMap);
        for (DWORD i = 0; i < map->cMap; ++i)
        {
            // A callstack fetch is the slowest host call, so an expired budget must not ship its result.
            if (requestExpired())
            {
                error = "request deadline exceeded";
                return std::nullopt;
            }
            const auto&      entry = map->pMap[i];
            ThreadDescriptor thread;
            thread.tid                = entry.dwTID;
            thread.pid                = entry.dwPID;
            thread.exitStatus         = entry.dwExitStatus;
            thread.userTime           = entry.dwUserTime;
            thread.kernelTime         = entry.dwKernelTime;
            thread.ethread            = entry.vaETHREAD;
            thread.teb                = entry.vaTeb;
            thread.startAddress       = entry.vaStartAddress;
            thread.win32StartAddress  = entry.vaWin32StartAddress;
            thread.stackBaseUser      = entry.vaStackBaseUser;
            thread.stackLimitUser     = entry.vaStackLimitUser;
            thread.stackBaseKernel    = entry.vaStackBaseKernel;
            thread.stackLimitKernel   = entry.vaStackLimitKernel;
            thread.trapFrame          = entry.vaTrapFrame;
            thread.instructionPointer = entry.vaRIP;
            thread.stackPointer       = entry.vaRSP;
            thread.affinity           = entry.qwAffinity;
            thread.createTime         = entry.ftCreateTime;
            thread.exitTime           = entry.ftExitTime;
            thread.impersonationToken = entry.vaImpersonationToken;
            thread.state              = entry.bState;
            thread.running            = entry.bRunning != 0;
            thread.priority           = entry.bPriority;
            thread.basePriority       = entry.bBasePriority;
            thread.suspendCount       = entry.bSuspendCount;
            thread.waitReason         = entry.bWaitReason;

            if ((request.flags & kThreadFlagCallstack) != 0 && matchesThreadFilter(request, thread))
            {
                appendCallstack(handle, request.pid, thread);
            }
            threads.push_back(std::move(thread));
        }
        return threads;
    }
} // namespace mpfshost
