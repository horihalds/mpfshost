#include "host/vmm_handle.hpp"

#include "core/debug/debug_controller.hpp"
#include "host/vmm_module_tables.hpp"
#include "host/vmm_pdb.hpp"

#include <algorithm>
#include <utility>

namespace mpfshost
{
    std::optional<DebugStatus> VmmHandle::debugState(bool& busy, std::string& error, bool& unavailable)
    {
        return _debug.state(busy, error, unavailable);
    }

    std::optional<DebugStatus>
    VmmHandle::debugControl(DebugControlAction action, bool& busy, std::string& error, bool& unavailable)
    {
        return _debug.control(action, busy, error, unavailable);
    }

    std::optional<std::vector<DebugRegisterRecord>> VmmHandle::debugRegisters(
        const DebugRegistersRequest& request, bool& invalidState, bool& busy, std::string& error, bool& unavailable)
    {
        return _debug.registers(request, invalidState, busy, error, unavailable);
    }

    std::optional<std::vector<DebugThreadRecord>> VmmHandle::debugThreads(
        const DebugThreadsRequest& request, bool& invalidState, bool& busy, std::string& error, bool& unavailable)
    {
        return _debug.threads(request, invalidState, busy, error, unavailable);
    }

    std::optional<DebugWatchEnd> VmmHandle::debugWatch(const DebugWatchRequest&    request,
                                                       const DebugWatchHitSink&    onHit,
                                                       const DebugWatchAbortCheck& aborted,
                                                       bool&                       busy,
                                                       std::string&                error,
                                                       bool&                       unavailable)
    {
        return _debug.watch(request, onHit, aborted, busy, error, unavailable);
    }

    std::optional<DebugBreakpointResponse> VmmHandle::debugBreakpoint(const DebugBreakpointRequest& request,
                                                                      const DebugWatchAbortCheck&   aborted,
                                                                      bool&                         invalidState,
                                                                      bool&                         busy,
                                                                      std::string&                  error,
                                                                      bool&                         unavailable)
    {
        return _debug.breakpoint(request, aborted, invalidState, busy, error, unavailable);
    }

    std::optional<DebugStepResponse> VmmHandle::debugStep(
        const DebugStepRequest& request, bool& invalidState, bool& busy, std::string& error, bool& unavailable)
    {
        return _debug.step(request, invalidState, busy, error, unavailable);
    }

    std::optional<DebugAttachResponse> VmmHandle::debugAttach(
        std::uint64_t sessionId, bool& invalidState, bool& busy, std::string& error, bool& unavailable)
    {
        return _debug.attach(sessionId, invalidState, busy, error, unavailable);
    }

    std::optional<DebugDetachResponse>
    VmmHandle::debugDetach(std::uint64_t sessionId, bool& busy, std::string& error, bool& unavailable)
    {
        return _debug.detach(sessionId, busy, error, unavailable);
    }

    void VmmHandle::debugDetachSession(std::uint64_t sessionId) noexcept
    {
        _debug.detachSession(sessionId);
    }

    std::optional<DebugBreakpointArmResponse> VmmHandle::debugArmBreakpoint(std::uint64_t                    sessionId,
                                                                            const DebugBreakpointArmRequest& request,
                                                                            bool&        invalidState,
                                                                            bool&        busy,
                                                                            std::string& error,
                                                                            bool&        unavailable)
    {
        return _debug.armBreakpoint(sessionId, request, invalidState, busy, error, unavailable);
    }

    std::optional<DebugResourceRemoveResponse>
    VmmHandle::debugRemoveBreakpoint(std::uint64_t                       sessionId,
                                     const DebugBreakpointRemoveRequest& request,
                                     bool&                               invalidState,
                                     bool&                               busy,
                                     std::string&                        error,
                                     bool&                               unavailable)
    {
        return _debug.removeBreakpoint(sessionId, request, invalidState, busy, error, unavailable);
    }

    std::optional<DebugBreakpointListResponse> VmmHandle::debugListBreakpoints(
        std::uint64_t sessionId, bool& invalidState, bool& busy, std::string& error, bool& unavailable)
    {
        return _debug.listBreakpoints(sessionId, invalidState, busy, error, unavailable);
    }

    std::optional<DebugWatchArmResponse> VmmHandle::debugArmWatch(std::uint64_t               sessionId,
                                                                  const DebugWatchArmRequest& request,
                                                                  bool&                       invalidState,
                                                                  bool&                       busy,
                                                                  std::string&                error,
                                                                  bool&                       unavailable)
    {
        return _debug.armWatch(sessionId, request, invalidState, busy, error, unavailable);
    }

    std::optional<DebugResourceRemoveResponse> VmmHandle::debugRemoveWatch(std::uint64_t                  sessionId,
                                                                           const DebugWatchRemoveRequest& request,
                                                                           bool&                          invalidState,
                                                                           bool&                          busy,
                                                                           std::string&                   error,
                                                                           bool&                          unavailable)
    {
        return _debug.removeWatch(sessionId, request, invalidState, busy, error, unavailable);
    }

    std::optional<DebugWatchListResponse> VmmHandle::debugListWatches(
        std::uint64_t sessionId, bool& invalidState, bool& busy, std::string& error, bool& unavailable)
    {
        return _debug.listWatches(sessionId, invalidState, busy, error, unavailable);
    }

    std::optional<DebugContinueResponse> VmmHandle::debugContinueGuest(
        std::uint64_t sessionId, bool& invalidState, bool& busy, std::string& error, bool& unavailable)
    {
        return _debug.continueGuest(sessionId, invalidState, busy, error, unavailable);
    }

    std::optional<DebugWaitEnd> VmmHandle::debugWait(const DebugWaitRequest&     request,
                                                     const DebugWaitHitSink&     onHit,
                                                     const DebugWatchAbortCheck& aborted,
                                                     std::uint64_t               sessionId,
                                                     bool&                       invalidState,
                                                     bool&                       busy,
                                                     std::string&                error,
                                                     bool&                       unavailable)
    {
        return _debug.wait(request, onHit, aborted, sessionId, invalidState, busy, error, unavailable);
    }

    std::optional<debug::DebugProcessDtbs> VmmHandle::processDtbs(std::uint32_t pid, std::string& error)
    {
        LookupStatus status = LookupStatus::Failed;
        const auto   info   = processInfo(ProcessInfoRequest {pid, {}}, status, error);
        if (!info)
        {
            if (status == LookupStatus::NotFound && error.empty())
            {
                error = "process " + std::to_string(pid) + " is not available";
            }
            return std::nullopt;
        }
        return debug::DebugProcessDtbs {info->dtb, info->dtbUser};
    }

    std::optional<debug::DebugModuleHit>
    VmmHandle::moduleForAddress(std::uint32_t pid, std::uint64_t address, std::string& error)
    {
        LookupStatus status = LookupStatus::Failed;
        const auto   module = moduleByAddress(ModuleByAddressRequest {pid, address}, status, error);
        if (!module)
        {
            error.clear();
            return std::nullopt;
        }
        return debug::DebugModuleHit {module->base, module->imageSize, module->offset, module->name};
    }

    debug::DebugExportHit
    VmmHandle::nearestExport(std::uint32_t pid, const std::string& module, std::uint64_t address, std::string& error)
    {
        LookupStatus status  = LookupStatus::Failed;
        const auto   exports = listExports(ExportListRequest {pid, module}, status, error);
        if (!exports)
        {
            error.clear();
            return {};
        }

        debug::DebugExportHit best;
        for (const auto& entry : exports->exports)
        {
            if (entry.vaFunction == 0 || entry.vaFunction > address)
            {
                continue;
            }
            if (!best.found || entry.vaFunction > best.address)
            {
                best.found   = true;
                best.address = entry.vaFunction;
                best.offset  = address - entry.vaFunction;
                best.name    = entry.function;
            }
        }
        return best;
    }

    std::optional<debug::DebugPdbHit>
    VmmHandle::pdbSymbol(std::uint32_t, const std::string& module, std::uint64_t offset, std::string& error)
    {
        LookupStatus status = LookupStatus::Failed;
        const auto   symbol = lookupVmmPdbSymbolName(_handle, PdbSymbolNameRequest {module, offset}, status, error);
        if (!symbol)
        {
            error.clear();
            return std::nullopt;
        }
        return debug::DebugPdbHit {symbol->symbol, symbol->displacement};
    }

    std::optional<debug::DebugGuestThread>
    VmmHandle::guestThread(std::uint64_t cr3, std::uint64_t rip, std::uint64_t rsp, std::string& error)
    {
        LookupStatus status  = LookupStatus::Failed;
        const auto   process = processByDtb(ProcessByDtbRequest {cr3}, status, error);
        if (!process)
        {
            error.clear();
            return std::nullopt;
        }

        debug::DebugGuestThread guest;
        guest.pid = process->process.pid;

        const auto threads = listThreads(ThreadListRequest {guest.pid, 0, 0}, error);
        if (!threads)
        {
            error.clear();
            return guest;
        }

        // A running and a paused vCPU both leave RSP inside their thread's stack. Prefer a thread whose trap
        // frame or user instruction pointer also matches RIP so a shared stack range cannot pick the wrong one.
        const ThreadDescriptor* best        = nullptr;
        bool                    bestIpMatch = false;
        for (const auto& thread : *threads)
        {
            const bool userStack   = thread.stackBaseUser != 0 && thread.stackLimitUser != 0
                                  && rsp >= thread.stackLimitUser && rsp < thread.stackBaseUser;
            const bool kernelStack = thread.stackBaseKernel != 0 && thread.stackLimitKernel != 0
                                  && rsp >= thread.stackLimitKernel && rsp < thread.stackBaseKernel;
            if (!userStack && !kernelStack)
            {
                continue;
            }
            const bool ipMatch = thread.instructionPointer == rip;
            if (best == nullptr || (ipMatch && !bestIpMatch))
            {
                best        = &thread;
                bestIpMatch = ipMatch;
                if (ipMatch)
                {
                    break;
                }
            }
        }
        if (best != nullptr)
        {
            guest.tid         = best->tid;
            guest.threadFound = true;
        }
        return guest;
    }
} // namespace mpfshost
