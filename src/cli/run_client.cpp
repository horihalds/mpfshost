#include "pch.hpp"

#include "cli/options.hpp"
#include "cli/render.hpp"
#include "cli/run_client.hpp"
#include "core/client/client.hpp"
#include "core/disasm/x86_decoder.hpp"
#include "core/log/log.hpp"
#include "core/util/display.hpp"
#include "core/util/hex.hpp"
#include "core/util/symbol_expression.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace mpfshost::cli
{
    namespace
    {
        void printScatterResults(const std::vector<mpfshost::ScatterReadResult>& results)
        {
            for (const auto& result : results)
            {
                std::printf("range 0x%016llx  size=%u  bytesRead=%u  %s\n",
                            static_cast<unsigned long long>(result.address),
                            static_cast<unsigned>(result.requestedSize),
                            static_cast<unsigned>(result.bytesRead),
                            result.ok ? "ok" : "failed");
                if (!result.data.empty())
                {
                    const auto dump = mpfshost::formatHexDump(result.data, result.address);
                    std::fputs(dump.c_str(), stdout);
                }
            }
        }

        void printGuestContext(std::uint32_t pid, std::uint32_t tid, bool processFound, bool threadFound)
        {
            if (processFound)
            {
                std::printf("  pid=%u", static_cast<unsigned>(pid));
            }
            if (threadFound)
            {
                std::printf("  tid=%u", static_cast<unsigned>(tid));
            }
        }

        void printDebugRecord(const mpfshost::DebugRegisterRecord& record)
        {
            std::printf("vcpu %u  thread %s", static_cast<unsigned>(record.vcpu), record.threadId.c_str());
            printGuestContext(record.guestPid, record.guestTid, record.processFound, record.threadFound);
            std::printf("\n");
            std::printf("  rip=0x%016llx  rsp=0x%016llx  rbp=0x%016llx  eflags=0x%08llx\n",
                        static_cast<unsigned long long>(record.rip),
                        static_cast<unsigned long long>(record.rsp),
                        static_cast<unsigned long long>(record.rbp),
                        static_cast<unsigned long long>(record.eflags));
            std::printf("  cr0=0x%016llx  cr3=0x%016llx  cr4=0x%016llx  cr8=0x%016llx\n",
                        static_cast<unsigned long long>(record.cr0),
                        static_cast<unsigned long long>(record.cr3),
                        static_cast<unsigned long long>(record.cr4),
                        static_cast<unsigned long long>(record.cr8));
            std::printf("  rax=0x%016llx  rbx=0x%016llx  rcx=0x%016llx  rdx=0x%016llx\n",
                        static_cast<unsigned long long>(record.rax),
                        static_cast<unsigned long long>(record.rbx),
                        static_cast<unsigned long long>(record.rcx),
                        static_cast<unsigned long long>(record.rdx));
            std::printf("  rsi=0x%016llx  rdi=0x%016llx  r8 =0x%016llx  r9 =0x%016llx\n",
                        static_cast<unsigned long long>(record.rsi),
                        static_cast<unsigned long long>(record.rdi),
                        static_cast<unsigned long long>(record.r8),
                        static_cast<unsigned long long>(record.r9));
            std::printf("  r10=0x%016llx  r11=0x%016llx  r12=0x%016llx  r13=0x%016llx\n",
                        static_cast<unsigned long long>(record.r10),
                        static_cast<unsigned long long>(record.r11),
                        static_cast<unsigned long long>(record.r12),
                        static_cast<unsigned long long>(record.r13));
            std::printf("  r14=0x%016llx  r15=0x%016llx\n",
                        static_cast<unsigned long long>(record.r14),
                        static_cast<unsigned long long>(record.r15));
        }

        void printDebugThread(const mpfshost::DebugThreadRecord& thread)
        {
            std::printf("vcpu %u  thread %s  rip=0x%016llx  rsp=0x%016llx  cr3=0x%016llx",
                        static_cast<unsigned>(thread.vcpu),
                        thread.threadId.c_str(),
                        static_cast<unsigned long long>(thread.rip),
                        static_cast<unsigned long long>(thread.rsp),
                        static_cast<unsigned long long>(thread.cr3));
            if (thread.processFound)
            {
                std::printf("  pid=%u", static_cast<unsigned>(thread.pid));
            }
            else
            {
                std::printf("  pid=?");
            }
            if (thread.threadFound)
            {
                std::printf("  tid=%u\n", static_cast<unsigned>(thread.tid));
            }
            else
            {
                std::printf("  tid=?\n");
            }
        }

        void printDebugArmBreakpoint(const mpfshost::DebugBreakpointArmResponse& breakpoint)
        {
            std::printf("breakpoint  id=%u  pid=%u  address=0x%016llx  kind=%s  ",
                        static_cast<unsigned>(breakpoint.id),
                        static_cast<unsigned>(breakpoint.pid),
                        static_cast<unsigned long long>(breakpoint.address),
                        breakpoint.kind == mpfshost::DebugBreakpointKind::Hardware ? "hardware" : "software");
            if (breakpoint.slot == mpfshost::kNoDebugHardwareSlot)
            {
                std::printf("slot=none\n");
            }
            else
            {
                std::printf("slot=%u\n", static_cast<unsigned>(breakpoint.slot));
            }
        }

        void printDebugArmWatch(const mpfshost::DebugWatchArmResponse& watch)
        {
            std::printf("watchpoint  id=%u  pid=%u  address=0x%016llx  width=%u  access=write  slot=%u\n",
                        static_cast<unsigned>(watch.id),
                        static_cast<unsigned>(watch.pid),
                        static_cast<unsigned long long>(watch.address),
                        static_cast<unsigned>(watch.width),
                        static_cast<unsigned>(watch.slot));
        }

        void printDebugStatus(const mpfshost::DebugStatus& status)
        {
            std::printf("run-state=%s  arch=%s  stub-active=%s  stub-owned=%s  foreign-stub=%s  host-paused=%s  "
                        "vcpus=%u  watches=%u\n",
                        std::string {mpfshost::debugRunStateName(status.runState)}.c_str(),
                        status.architecture.c_str(),
                        status.stubActive ? "yes" : "no",
                        status.stubOwned ? "yes" : "no",
                        status.foreignStub ? "yes" : "no",
                        status.hostPaused ? "yes" : "no",
                        static_cast<unsigned>(status.vcpuCount),
                        static_cast<unsigned>(status.watchCount));
        }
    } // namespace

    int runClient(const ClientOptions& options)
    {
        std::string error;
        if (options.command == "list-targets")
        {
            const auto response = mpfshost::listTargets(options.socketPath, error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            if (response->targets.empty())
            {
                std::printf("no VM targets are available\n");
                return 0;
            }

            std::printf("%-24s %8s  %s\n", "NAME", "PID", "QMP SOCKET");
            for (const auto& target : response->targets)
            {
                std::printf("%-24s %8u  %s\n",
                            target.name.c_str(),
                            static_cast<unsigned>(target.qemuPid),
                            target.qmpSocketPath.c_str());
            }
            return 0;
        }

        if (options.command == "capabilities")
        {
            const auto response = mpfshost::getCapabilities(options.socketPath, error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            std::printf("version=%s\n", response->serverVersion.c_str());
            std::printf("%-32s %s\n", "REQUEST", "VALUE");
            for (const auto type : response->requestTypes)
            {
                const auto name = std::string(mpfshost::messageTypeName(static_cast<mpfshost::MessageType>(type)));
                std::printf("%-32s 0x%04x\n", name.c_str(), static_cast<unsigned>(type));
            }
            return 0;
        }

        if (options.command == "processes")
        {
            const auto response =
                mpfshost::listProcesses(options.socketPath, {options.target, options.pid, options.nameFilter}, error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            if (response->processes.empty())
            {
                std::printf("no matching processes\n");
                return 0;
            }

            std::printf("%8s %8s %5s  %s\n", "PID", "PPID", "WOW64", "NAME");
            for (const auto& process : response->processes)
            {
                std::printf("%8u %8u %5s  %s\n",
                            static_cast<unsigned>(process.pid),
                            static_cast<unsigned>(process.parentPid),
                            process.wow64 ? "yes" : "no",
                            process.name.c_str());
            }
            return 0;
        }

        if (options.command == "processes-all")
        {
            const auto response = mpfshost::listAllProcesses(
                options.socketPath, {options.target, options.pid, options.nameFilter}, error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            if (response->processes.empty())
            {
                std::printf("no matching processes\n");
                return 0;
            }

            std::printf("%8s %8s %5s %6s  %s\n", "PID", "PPID", "WOW64", "STATE", "NAME");
            for (const auto& summary : response->processes)
            {
                const auto& process = summary.process;
                std::printf("%8u %8u %5s %6u  %s\n",
                            static_cast<unsigned>(process.pid),
                            static_cast<unsigned>(process.parentPid),
                            process.wow64 ? "yes" : "no",
                            static_cast<unsigned>(summary.state),
                            process.name.c_str());
            }
            return 0;
        }

        if (options.command == "proc-info")
        {
            const auto response =
                mpfshost::processInfo(options.socketPath, {options.target, options.pid, options.nameFilter}, error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            std::printf("pid=%u\n", static_cast<unsigned>(response->process.pid));
            std::printf("ppid=%u\n", static_cast<unsigned>(response->process.parentPid));
            std::printf("name=%s\n", response->process.name.c_str());
            std::printf("wow64=%s\n", response->process.wow64 ? "yes" : "no");
            std::printf("state=%u\n", static_cast<unsigned>(response->state));
            std::printf("memory-model=%u\n", static_cast<unsigned>(response->memoryModel));
            std::printf("system-type=%u\n", static_cast<unsigned>(response->systemType));
            std::printf("user-only=%s\n", response->userOnly ? "yes" : "no");
            std::printf("dtb=%s\n", hexPointer(response->dtb).c_str());
            std::printf("dtb-user=%s\n", hexPointer(response->dtbUser).c_str());
            std::printf("eprocess=%s\n", hexPointer(response->eprocessVa).c_str());
            std::printf("peb=%s\n", hexPointer(response->pebVa).c_str());
            if (response->peb32Va != 0)
            {
                std::printf("peb32=0x%08x\n", response->peb32Va);
            }
            std::printf("session=%u\n", static_cast<unsigned>(response->sessionId));
            std::printf("luid=%s\n", hexPointer(response->luid).c_str());
            std::printf("integrity=%u\n", static_cast<unsigned>(response->integrityLevel));
            std::printf("sid=%s\n", response->sid.c_str());
            std::printf("image=%s\n", response->imagePath.c_str());
            std::printf("kernel=%s\n", response->kernelPath.c_str());
            std::printf("cmdline=%s\n", response->cmdline.c_str());
            return 0;
        }

        if (options.command == "process-for-dtb")
        {
            const auto response = mpfshost::processByDtb(options.socketPath, {options.target, options.dtb}, error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            std::printf("pid=%u\n", static_cast<unsigned>(response->process.pid));
            std::printf("ppid=%u\n", static_cast<unsigned>(response->process.parentPid));
            std::printf("name=%s\n", response->process.name.c_str());
            std::printf("wow64=%s\n", response->process.wow64 ? "yes" : "no");
            std::printf("dtb=%s\n", hexPointer(response->dtb).c_str());
            std::printf("dtb-user=%s\n", hexPointer(response->dtbUser).c_str());
            std::printf("matched=%s\n", response->matchedUser ? "dtb-user" : "dtb");
            return 0;
        }

        if (options.command == "module-at")
        {
            const auto response =
                mpfshost::moduleByAddress(options.socketPath, {options.target, options.pid, options.address}, error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            std::printf("name=%s\n", response->name.c_str());
            std::printf("full-name=%s\n", response->fullName.c_str());
            std::printf("base=%s\n", hexPointer(response->base).c_str());
            std::printf("entry=%s\n", hexPointer(response->entry).c_str());
            std::printf("size=%s\n", hexPointer(response->imageSize).c_str());
            std::printf("offset=%s\n", hexPointer(response->offset).c_str());
            std::printf("kind=%u\n", static_cast<unsigned>(response->kind));
            std::printf("wow64=%s\n", response->wow64 ? "yes" : "no");
            return 0;
        }

        if (options.command == "resolve")
        {
            const auto response =
                mpfshost::resolveAddress(options.socketPath, {options.target, options.pid, options.address}, error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            std::printf("address=%s\n", hexPointer(options.address).c_str());
            if (!response->moduleFound)
            {
                std::printf("module=<none>\n");
                return 0;
            }
            std::printf("module=%s\n", response->module.c_str());
            std::printf("full-name=%s\n", response->moduleFullName.c_str());
            std::printf("base=%s\n", hexPointer(response->moduleBase).c_str());
            std::printf("offset=%s\n", hexPointer(response->moduleOffset).c_str());
            if (response->exportFound)
            {
                std::printf("export=%s\n", response->exportName.empty() ? "?" : response->exportName.c_str());
                std::printf("export-address=%s\n", hexPointer(response->exportAddress).c_str());
                std::printf("export-offset=%s\n", hexPointer(response->exportOffset).c_str());
            }
            else
            {
                std::printf("export=<none>\n");
            }
            if (response->pdbFound)
            {
                std::printf("symbol=%s\n", response->pdbSymbol.c_str());
                std::printf("symbol-displacement=0x%x\n", static_cast<unsigned>(response->pdbDisplacement));
            }
            else
            {
                std::printf("symbol=<none>\n");
            }
            return 0;
        }

        if (options.command == "symbol")
        {
            const auto response = mpfshost::lookupSymbol(
                options.socketPath, {options.target, options.pid, options.moduleName, options.symbolName}, error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            std::printf("module=%s\n", options.moduleName.c_str());
            std::printf("base=%s\n", hexPointer(response->moduleBase).c_str());
            if (options.symbolName.empty())
            {
                return 0;
            }
            if (!response->found)
            {
                std::printf("symbol=%s not found\n", options.symbolName.c_str());
                return 0;
            }
            std::printf("symbol=%s\n", options.symbolName.c_str());
            std::printf("address=%s\n", hexPointer(response->address).c_str());
            std::printf("offset=%s\n", hexPointer(response->offset).c_str());
            return 0;
        }

        if (options.command == "pdb-load")
        {
            const auto response =
                mpfshost::pdbLoad(options.socketPath, {options.target, options.pid, options.address}, error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            std::printf("module=%s\n", response->module.c_str());
            return 0;
        }

        if (options.command == "pdb-symbol")
        {
            const auto response = mpfshost::pdbSymbolName(
                options.socketPath, {options.target, options.moduleName, options.address}, error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            std::printf("module=%s\n", options.moduleName.c_str());
            std::printf("symbol=%s\n", response->symbol.c_str());
            std::printf("displacement=0x%x\n", static_cast<unsigned>(response->displacement));
            return 0;
        }

        if (options.command == "pdb-address")
        {
            const auto response = mpfshost::pdbSymbolAddress(
                options.socketPath, {options.target, options.moduleName, options.symbolName}, error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            std::printf("module=%s\n", options.moduleName.c_str());
            std::printf("symbol=%s\n", options.symbolName.c_str());
            std::printf("address=%s\n", hexPointer(response->address).c_str());
            return 0;
        }

        if (options.command == "pdb-type-size")
        {
            const auto response = mpfshost::pdbTypeSize(
                options.socketPath, {options.target, options.moduleName, options.typeName}, error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            std::printf("module=%s\n", options.moduleName.c_str());
            std::printf("type=%s\n", options.typeName.c_str());
            std::printf("size=%u\n", static_cast<unsigned>(response->size));
            return 0;
        }

        if (options.command == "pdb-type-child")
        {
            const auto response = mpfshost::pdbTypeChildOffset(
                options.socketPath, {options.target, options.moduleName, options.typeName, options.childName}, error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            std::printf("module=%s\n", options.moduleName.c_str());
            std::printf("type=%s\n", options.typeName.c_str());
            std::printf("child=%s\n", options.childName.c_str());
            std::printf("offset=0x%x\n", static_cast<unsigned>(response->offset));
            return 0;
        }

        if (options.command == "sections")
        {
            const auto response =
                mpfshost::moduleLayout(options.socketPath, {options.target, options.pid, options.moduleName}, error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            std::printf("module=%s\n", options.moduleName.c_str());
            std::printf("base=%s\n", hexPointer(response->moduleBase).c_str());
            std::printf("%-10s %18s %10s %10s %10s %10s\n", "NAME", "VA", "VSIZE", "RAW", "RAWSIZE", "CHARACTERISTICS");
            for (const auto& section : response->sections)
            {
                std::printf("%-10s %18s %10u %10u %10u %10x\n",
                            section.name.c_str(),
                            hexPointer(section.virtualAddress).c_str(),
                            static_cast<unsigned>(section.virtualSize),
                            static_cast<unsigned>(section.rawPointer),
                            static_cast<unsigned>(section.rawSize),
                            static_cast<unsigned>(section.characteristics));
            }
            std::printf("%-6s %12s %10s\n", "DIR#", "RVA", "SIZE");
            for (const auto& directory : response->directories)
            {
                std::printf("%-6u 0x%08x %10u\n",
                            static_cast<unsigned>(directory.index),
                            directory.virtualAddress,
                            static_cast<unsigned>(directory.size));
            }
            return 0;
        }

        if (options.command == "modules")
        {
            const auto modules = mpfshost::listModules(
                options.socketPath, {options.target, options.pid, options.nameFilter, options.moduleFlags}, error);
            if (!modules)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            if (modules->empty())
            {
                std::printf("no matching modules\n");
                return 0;
            }

            std::printf("%-18s %-18s %10s %-9s %5s  %s\n", "BASE", "ENTRY", "IMAGE", "KIND", "WOW64", "NAME");
            for (const auto& module : *modules)
            {
                std::printf("0x%016llx 0x%016llx 0x%08x %-9s %5s  %s",
                            static_cast<unsigned long long>(module.base),
                            static_cast<unsigned long long>(module.entry),
                            module.imageSize,
                            moduleKindName(module.kind),
                            module.wow64 ? "yes" : "no",
                            module.name.c_str());
                if (!module.fullName.empty() && module.fullName != module.name)
                {
                    std::printf("  (%s)", module.fullName.c_str());
                }
                if ((options.moduleFlags & mpfshost::kModuleFlagCounts) != 0)
                {
                    std::printf("  raw=%u sections=%u eat=%u iat=%u",
                                module.rawFileSize,
                                static_cast<unsigned>(module.sectionCount),
                                module.eatCount,
                                module.iatCount);
                }
                if ((options.moduleFlags & mpfshost::kModuleFlagDebugInfo) != 0)
                {
                    std::printf("  pdb=%s %s", module.pdbGuid.c_str(), module.pdbName.c_str());
                }
                if ((options.moduleFlags & mpfshost::kModuleFlagVersionInfo) != 0)
                {
                    std::printf("  version=%s", module.version.c_str());
                }
                std::printf("\n");
            }
            return 0;
        }

        if (options.command == "maps")
        {
            const auto regions = mpfshost::listMemoryMap(
                options.socketPath, {options.target, options.pid, options.identifyModules}, error);
            if (!regions)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            if (regions->empty())
            {
                std::printf("no memory map regions\n");
                return 0;
            }

            std::printf(
                "%-18s %-18s %4s %4s %-20s %4s  %s\n", "START", "END", "PROT", "TYPE", "FLAGS", "HEAP", "DESCRIPTION");
            for (const auto& region : *regions)
            {
                std::printf("0x%016llx 0x%016llx 0x%02x %4u %-20s %4u  %s\n",
                            static_cast<unsigned long long>(region.start),
                            static_cast<unsigned long long>(region.end),
                            region.protection,
                            static_cast<unsigned>(region.vadType),
                            mapFlagNames(region.flags).c_str(),
                            static_cast<unsigned>(region.heapNumber),
                            region.description.c_str());
            }
            return 0;
        }

        if (options.command == "vadex")
        {
            const auto pages = mpfshost::listVadEx(
                options.socketPath, {options.target, options.pid, options.pageOffset, options.pageCount}, error);
            if (!pages)
            {
                mpfshost::log::error("client", "vad-ex listing failed: ", error);
                return 1;
            }

            if (pages->empty())
            {
                std::printf("no vad-ex pages\n");
                return 0;
            }

            std::printf("%18s %18s %18s %-11s %3s %-8s %-11s %18s %18s %18s\n",
                        "VA",
                        "PA",
                        "PTE",
                        "TYPE",
                        "PML",
                        "FLAGS",
                        "PROTO-TYPE",
                        "PROTO-PA",
                        "PROTO-PTE",
                        "VAD");
            for (const auto& page : *pages)
            {
                std::printf("0x%016llx 0x%016llx 0x%016llx %-11s %3u %-8s %-11s 0x%016llx 0x%016llx 0x%016llx\n",
                            static_cast<unsigned long long>(page.va),
                            static_cast<unsigned long long>(page.pa),
                            static_cast<unsigned long long>(page.pte),
                            pteTypeName(page.type),
                            static_cast<unsigned>(page.pml),
                            vadExFlagNames(page.flags).c_str(),
                            pteTypeName(page.protoType),
                            static_cast<unsigned long long>(page.protoPa),
                            static_cast<unsigned long long>(page.protoPte),
                            static_cast<unsigned long long>(page.vaVadBase));
            }
            return 0;
        }

        if (options.command == "threads")
        {
            std::uint16_t flags = 0;
            if (options.callstack)
            {
                flags |= mpfshost::kThreadFlagCallstack;
            }

            const auto threads =
                mpfshost::listThreads(options.socketPath, {options.target, options.pid, options.tid, flags}, error);
            if (!threads)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            if (threads->empty())
            {
                std::printf("no matching threads\n");
                return 0;
            }

            std::printf("%7s %7s %5s %3s %3s %3s %4s %7s %6s %16s %18s %18s %18s\n",
                        "TID",
                        "PID",
                        "STATE",
                        "RUN",
                        "PRI",
                        "SUS",
                        "WAIT",
                        "USERms",
                        "KERNms",
                        "AFFINITY",
                        "START",
                        "RIP",
                        "ETHREAD");
            for (const auto& thread : *threads)
            {
                std::printf("%7u %7u %5u %3s %3u %3u %4u %7u %6u 0x%014llx 0x%016llx %18s %18s\n",
                            static_cast<unsigned>(thread.tid),
                            static_cast<unsigned>(thread.pid),
                            static_cast<unsigned>(thread.state),
                            thread.running ? "yes" : "no",
                            static_cast<unsigned>(thread.priority),
                            static_cast<unsigned>(thread.suspendCount),
                            static_cast<unsigned>(thread.waitReason),
                            static_cast<unsigned>(thread.userTime),
                            static_cast<unsigned>(thread.kernelTime),
                            static_cast<unsigned long long>(thread.affinity),
                            static_cast<unsigned long long>(thread.startAddress),
                            hexPointer(thread.instructionPointer).c_str(),
                            hexPointer(thread.ethread).c_str());

                if (!options.callstack)
                {
                    continue;
                }
                if (thread.callstackStatus != 0)
                {
                    std::printf("    <callstack unavailable>\n");
                    continue;
                }
                for (const auto& frame : thread.frames)
                {
                    // MemProcFS leaves the module and function empty for a frame outside every mapped image,
                    // so the bare address is printed instead of an empty "!+0x0" symbol.
                    if (frame.module.empty())
                    {
                        std::printf("    #%u rip=0x%016llx\n",
                                    static_cast<unsigned>(frame.index),
                                    static_cast<unsigned long long>(frame.returnAddress));
                        continue;
                    }
                    std::printf("    #%u rip=0x%016llx %s%s%s+0x%x\n",
                                static_cast<unsigned>(frame.index),
                                static_cast<unsigned long long>(frame.returnAddress),
                                frame.module.c_str(),
                                frame.function.empty() ? "" : "!",
                                frame.function.c_str(),
                                static_cast<unsigned>(frame.displacement));
                }
            }
            return 0;
        }

        if (options.command == "handles")
        {
            const auto handles =
                mpfshost::listHandles(options.socketPath, {options.target, options.pid, options.nameFilter}, error);
            if (!handles)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            if (handles->empty())
            {
                std::printf("no matching handles\n");
                return 0;
            }

            std::printf("%10s %-16s %-8s %10s %18s %18s %18s %4s  %s\n",
                        "HANDLE",
                        "TYPE",
                        "ACCESS",
                        "TAG",
                        "OBJECT",
                        "CREATEINFO",
                        "SD",
                        "HCNT",
                        "TEXT");
            for (const auto& handle : *handles)
            {
                std::printf("0x%08x %-16s 0x%06x 0x%08x 0x%016llx 0x%016llx 0x%016llx %4llu  %s\n",
                            handle.handle,
                            handle.type.empty() ? "-" : handle.type.c_str(),
                            handle.grantedAccess,
                            handle.poolTag,
                            static_cast<unsigned long long>(handle.vaObject),
                            static_cast<unsigned long long>(handle.objectCreateInfo),
                            static_cast<unsigned long long>(handle.securityDescriptor),
                            static_cast<unsigned long long>(handle.handleCount),
                            handle.text.c_str());
            }
            return 0;
        }

        if (options.command == "eat" || options.command == "iat")
        {
            if (options.command == "eat")
            {
                const auto exports =
                    mpfshost::listExports(options.socketPath, {options.target, options.pid, options.moduleName}, error);
                if (!exports)
                {
                    mpfshost::log::error("client", error);
                    return 1;
                }

                if (exports->empty())
                {
                    std::printf("no exports\n");
                    return 0;
                }

                std::printf("%18s %6s %8s %8s  %s\n", "FUNCTION", "ORD", "FNARR", "NARR", "NAME");
                for (const auto& entry : *exports)
                {
                    std::printf("0x%016llx %6u %8u %8s  %s",
                                static_cast<unsigned long long>(entry.vaFunction),
                                entry.ordinal,
                                entry.functionsArrayOffset,
                                mpfshost::exportNamesArrayText(entry.namesArrayOffset).c_str(),
                                entry.function.c_str());
                    if (!entry.forwardedFunction.empty())
                    {
                        std::printf("  -> %s", entry.forwardedFunction.c_str());
                    }
                    std::printf("\n");
                }
                return 0;
            }

            const auto imports =
                mpfshost::listImports(options.socketPath, {options.target, options.pid, options.moduleName}, error);
            if (!imports)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            if (imports->empty())
            {
                std::printf("no imports\n");
                return 0;
            }

            std::printf(
                "%18s %3s %5s %8s %8s  %-24s %s\n", "FUNCTION", "32B", "HINT", "THUNK", "OTHUNK", "MODULE", "NAME");
            for (const auto& entry : *imports)
            {
                std::printf("0x%016llx %3s %5u %8u %8u  %-24s %s\n",
                            static_cast<unsigned long long>(entry.vaFunction),
                            entry.is32Bit ? "yes" : "no",
                            static_cast<unsigned>(entry.hint),
                            entry.firstThunkRva,
                            entry.originalFirstThunkRva,
                            entry.module.c_str(),
                            entry.function.c_str());
            }
            return 0;
        }

        if (options.command == "unloaded")
        {
            const auto modules = mpfshost::listUnloadedModules(
                options.socketPath, {options.target, options.pid, options.nameFilter}, error);
            if (!modules)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            if (modules->empty())
            {
                std::printf("no matching unloaded modules\n");
                return 0;
            }

            std::printf("%18s %10s %4s %10s %10s %18s  %s\n",
                        "BASE",
                        "IMAGE",
                        "WOW",
                        "CHECKSUM",
                        "TIMESTAMP",
                        "UNLOAD",
                        "NAME");
            for (const auto& module : *modules)
            {
                std::printf("0x%016llx 0x%08x %4s 0x%08x 0x%08x 0x%016llx  %s\n",
                            static_cast<unsigned long long>(module.vaBase),
                            module.imageSize,
                            module.isWow64 ? "yes" : "no",
                            module.checksum,
                            module.timeDateStamp,
                            static_cast<unsigned long long>(module.unloadTime),
                            module.text.c_str());
            }
            return 0;
        }

        if (options.command == "services")
        {
            const auto answer = mpfshost::listServices(options.socketPath, {options.target, options.nameFilter}, error);
            if (!answer)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            if (answer->services.empty())
            {
                std::printf("no matching services\n");
                return 0;
            }

            std::printf("%18s %5s %5s %5s %5s %5s %8s %8s %6s %8s %6s  %s\n",
                        "OBJECT",
                        "ORD",
                        "START",
                        "TYPE",
                        "STATE",
                        "CTRL",
                        "EXIT",
                        "SEXIT",
                        "CHK",
                        "WAIT",
                        "PID",
                        "NAME | DISPLAY | IMAGE | PATH | USERTP | ACCOUNT");
            for (const auto& service : answer->services)
            {
                std::printf("0x%016llx %5u %5u %5u %5u %5u %8u %8u %6u %8u %6u  %s | %s | %s | %s | %s | %s\n",
                            static_cast<unsigned long long>(service.objectAddress),
                            static_cast<unsigned>(service.ordinal),
                            static_cast<unsigned>(service.startType),
                            static_cast<unsigned>(service.serviceType),
                            static_cast<unsigned>(service.state),
                            static_cast<unsigned>(service.controlsAccepted),
                            static_cast<unsigned>(service.win32ExitCode),
                            static_cast<unsigned>(service.serviceSpecificExitCode),
                            static_cast<unsigned>(service.checkPoint),
                            static_cast<unsigned>(service.waitHint),
                            static_cast<unsigned>(service.pid),
                            service.name.empty() ? "-" : service.name.c_str(),
                            service.displayName.empty() ? "-" : service.displayName.c_str(),
                            service.imagePath.empty() ? "-" : service.imagePath.c_str(),
                            service.path.empty() ? "-" : service.path.c_str(),
                            service.userType.empty() ? "-" : service.userType.c_str(),
                            service.account.empty() ? "-" : service.account.c_str());
            }
            if (answer->truncated)
            {
                mpfshost::log::warn("client", "the service answer was truncated by the host entry cap");
            }
            return 0;
        }

        if (options.command == "users")
        {
            const auto answer = mpfshost::listUsers(options.socketPath, {options.target, options.nameFilter}, error);
            if (!answer)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            if (answer->users.empty())
            {
                std::printf("no matching users\n");
                return 0;
            }

            std::printf("%-32s %-48s %18s\n", "NAME", "SID", "HIVE");
            for (const auto& user : answer->users)
            {
                std::printf("%-32s %-48s 0x%016llx\n",
                            user.name.empty() ? "-" : user.name.c_str(),
                            user.sid.empty() ? "-" : user.sid.c_str(),
                            static_cast<unsigned long long>(user.regHive));
            }
            if (answer->truncated)
            {
                mpfshost::log::warn("client", "the user answer was truncated by the host entry cap");
            }
            return 0;
        }

        if (options.command == "net")
        {
            const auto answer =
                mpfshost::listNetwork(options.socketPath, {options.target, options.pid, options.nameFilter}, error);
            if (!answer)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            if (answer->entries.empty())
            {
                std::printf("no matching connections\n");
                return 0;
            }

            std::printf("%7s %-24s %5s %4s %-28s %-28s %18s %18s %8s\n",
                        "PID",
                        "PROCESS",
                        "STATE",
                        "AF",
                        "LOCAL",
                        "REMOTE",
                        "OBJECT",
                        "CREATED",
                        "TAG");
            for (const auto& entry : answer->entries)
            {
                std::printf("%7u %-24s %5u %4u %-28s %-28s 0x%016llx 0x%016llx 0x%08x\n",
                            static_cast<unsigned>(entry.pid),
                            entry.process.empty() ? "-" : entry.process.c_str(),
                            static_cast<unsigned>(entry.state),
                            static_cast<unsigned>(entry.addressFamily),
                            entry.local.valid ? (entry.local.address.empty() ? "-" : entry.local.address.c_str()) : "-",
                            entry.remote.valid ? (entry.remote.address.empty() ? "-" : entry.remote.address.c_str())
                                               : "-",
                            static_cast<unsigned long long>(entry.objectAddress),
                            static_cast<unsigned long long>(entry.createdTime),
                            static_cast<unsigned>(entry.poolTag));
            }
            if (answer->truncated)
            {
                mpfshost::log::warn("client", "the connection answer was truncated by the host entry cap");
            }
            return 0;
        }

        if (options.command == "heap")
        {
            const auto answer = mpfshost::listHeaps(options.socketPath, {options.target, options.pid}, error);
            if (!answer)
            {
                mpfshost::log::error("client", "heap listing failed: ", error);
                return 1;
            }

            if (answer->heaps.empty())
            {
                std::printf("no heaps\n");
                return 0;
            }

            std::printf("heap pid=%u: %zu heaps\n", static_cast<unsigned>(options.pid), answer->heaps.size());
            std::printf("%5s %6s %-8s %4s %18s\n", "INDEX", "NUMBER", "TYPE", "32BIT", "ADDRESS");
            for (const auto& heap : answer->heaps)
            {
                std::printf("%5u %6u %-8s %4s 0x%016llx\n",
                            static_cast<unsigned>(heap.index),
                            static_cast<unsigned>(heap.number),
                            heapTypeName(heap.type),
                            heap.is32Bit != 0 ? "yes" : "no",
                            static_cast<unsigned long long>(heap.address));
            }
            if (answer->truncated)
            {
                mpfshost::log::warn("client", "the heap answer was truncated by the host entry cap");
            }
            return 0;
        }

        if (options.command == "heap-alloc")
        {
            const auto answer =
                mpfshost::listHeapAllocations(options.socketPath, {options.target, options.pid, options.heap}, error);
            if (!answer)
            {
                mpfshost::log::error("client", "heap allocation listing failed: ", error);
                return 1;
            }

            if (answer->allocations.empty())
            {
                std::printf("no allocations\n");
                return 0;
            }

            std::printf("heap-alloc pid=%u heap=%s: %zu allocations\n",
                        static_cast<unsigned>(options.pid),
                        options.heapText.c_str(),
                        answer->allocations.size());
            std::printf("%18s %12s %-9s\n", "ADDRESS", "SIZE", "TYPE");
            for (const auto& allocation : answer->allocations)
            {
                std::printf("0x%016llx %12u %-9s\n",
                            static_cast<unsigned long long>(allocation.address),
                            allocation.size,
                            heapAllocTypeName(allocation.type));
            }
            if (answer->truncated)
            {
                mpfshost::log::warn("client", "the heap allocation answer was truncated by the host entry cap");
            }
            return 0;
        }

        if (options.command == "kdriver")
        {
            const auto answer =
                mpfshost::listKernelDrivers(options.socketPath, {options.target, options.nameFilter}, error);
            if (!answer)
            {
                mpfshost::log::error("client", "kernel driver listing failed: ", error);
                return 1;
            }

            if (answer->drivers.empty())
            {
                std::printf("no matching kernel drivers\n");
                return 0;
            }

            std::printf(
                "%18s %18s %10s %18s %4s  %s\n", "OBJECT", "BASE", "SIZE", "DEVICE", "IRPS", "NAME | SERVICE | PATH");
            for (const auto& driver : answer->drivers)
            {
                std::size_t irps = 0;
                for (const auto function : driver.majorFunctions)
                {
                    if (function != 0)
                    {
                        ++irps;
                    }
                }
                std::printf("0x%016llx 0x%016llx %10llu 0x%016llx %4zu  %s | %s | %s\n",
                            static_cast<unsigned long long>(driver.va),
                            static_cast<unsigned long long>(driver.vaDriverStart),
                            static_cast<unsigned long long>(driver.cbDriverSize),
                            static_cast<unsigned long long>(driver.vaDeviceObject),
                            irps,
                            driver.name.empty() ? "-" : driver.name.c_str(),
                            driver.serviceKeyName.empty() ? "-" : driver.serviceKeyName.c_str(),
                            driver.path.empty() ? "-" : driver.path.c_str());
            }
            if (answer->truncated)
            {
                mpfshost::log::warn("client", "the kernel driver answer was truncated by the host entry cap");
            }
            return 0;
        }

        if (options.command == "kdevice")
        {
            const auto answer =
                mpfshost::listKernelDevices(options.socketPath, {options.target, options.nameFilter}, error);
            if (!answer)
            {
                mpfshost::log::error("client", "kernel device listing failed: ", error);
                return 1;
            }

            if (answer->devices.empty())
            {
                std::printf("no matching kernel devices\n");
                return 0;
            }

            std::printf("%18s %5s %10s %-28s %18s %18s %18s  %s\n",
                        "OBJECT",
                        "DEPTH",
                        "TYPE",
                        "TYPE_NAME",
                        "DRIVER",
                        "ATTACHED",
                        "FSDEVICE",
                        "VOLUME");
            for (const auto& device : answer->devices)
            {
                std::printf("0x%016llx %5u %10u %-28s 0x%016llx 0x%016llx 0x%016llx  %s\n",
                            static_cast<unsigned long long>(device.va),
                            static_cast<unsigned>(device.depth),
                            static_cast<unsigned>(device.deviceType),
                            mpfshost::kernelDeviceTypeText(device.deviceTypeName).c_str(),
                            static_cast<unsigned long long>(device.vaDriverObject),
                            static_cast<unsigned long long>(device.vaAttachedDevice),
                            static_cast<unsigned long long>(device.vaFileSystemDevice),
                            device.volumeInfo.empty() ? "-" : device.volumeInfo.c_str());
            }
            if (answer->truncated)
            {
                mpfshost::log::warn("client", "the kernel device answer was truncated by the host entry cap");
            }
            return 0;
        }

        if (options.command == "kobject")
        {
            const auto answer =
                mpfshost::listKernelObjects(options.socketPath, {options.target, options.nameFilter}, error);
            if (!answer)
            {
                mpfshost::log::error("client", "kernel object listing failed: ", error);
                return 1;
            }

            if (answer->objects.empty())
            {
                std::printf("no matching kernel objects\n");
                return 0;
            }

            std::printf("%18s %18s %-20s %-48s %s\n", "OBJECT", "PARENT", "TYPE", "NAME", "CHILDREN");
            for (const auto& object : answer->objects)
            {
                const std::string children = object.childrenTruncated ? std::to_string(object.children.size()) + "/"
                                                                            + std::to_string(object.childCount)
                                                                      : std::to_string(object.children.size());
                std::printf("0x%016llx 0x%016llx %-20s %-48s %s\n",
                            static_cast<unsigned long long>(object.va),
                            static_cast<unsigned long long>(object.vaParent),
                            object.type.empty() ? "-" : object.type.c_str(),
                            object.name.empty() ? "-" : object.name.c_str(),
                            children.c_str());
            }
            if (answer->truncated)
            {
                mpfshost::log::warn("client", "the kernel object answer was truncated by the host entry cap");
            }
            return 0;
        }

        if (options.command == "pte")
        {
            const auto answer = mpfshost::listPte(options.socketPath,
                                                  {options.target,
                                                   options.pid,
                                                   options.identifyModules,
                                                   options.nameFilter,
                                                   options.minAddress,
                                                   options.maxAddress},
                                                  error);
            if (!answer)
            {
                mpfshost::log::error("client", "pte listing failed: ", error);
                return 1;
            }

            if (answer->regions.empty())
            {
                std::printf("no matching pte regions\n");
                return 0;
            }

            std::printf("%18s %10s %-10s %8s %5s  %s\n", "BASE", "PAGES", "FLAGS", "SOFTWARE", "WOW64", "TEXT");
            for (const auto& region : answer->regions)
            {
                std::printf("%18s %10llu %-10s %8u %5u  %s\n",
                            hexPointer(region.vaBase).c_str(),
                            static_cast<unsigned long long>(region.pages),
                            pteFlagNames(region.pageFlags).c_str(),
                            static_cast<unsigned>(region.softwarePages),
                            static_cast<unsigned>(region.wow64),
                            region.text.empty() ? "-" : region.text.c_str());
            }
            std::printf("%zu of %llu pte regions%s\n",
                        answer->regions.size(),
                        static_cast<unsigned long long>(answer->mapEntries),
                        answer->truncated ? ", truncated" : "");
            return 0;
        }

        if (options.command == "pool")
        {
            const auto answer = mpfshost::listPool(options.socketPath,
                                                   {options.target,
                                                    options.bigPool,
                                                    options.tagFilter,
                                                    options.minBytes,
                                                    options.minAddress,
                                                    options.maxAddress},
                                                   error);
            if (!answer)
            {
                mpfshost::log::error("client", "pool listing failed: ", error);
                return 1;
            }

            if (answer->entries.empty())
            {
                std::printf("no matching pool entries\n");
                return 0;
            }

            std::printf("%18s %10s %-4s %-5s %-14s %s\n", "ADDRESS", "SIZE", "TAG", "ALLOC", "TYPE", "SUBSEGMENT");
            for (const auto& entry : answer->entries)
            {
                std::printf("%18s %10u %-4s %-5s %-14s %s\n",
                            hexPointer(entry.va).c_str(),
                            static_cast<unsigned>(entry.bytes),
                            poolTagText(entry.tag).c_str(),
                            entry.alloc != 0 ? "alloc" : "free",
                            poolTypeName(entry.poolType),
                            poolSubsegmentName(entry.subsegment));
            }
            std::printf("%zu of %llu pool entries (%llu bytes)%s\n",
                        answer->entries.size(),
                        static_cast<unsigned long long>(answer->mapEntries),
                        static_cast<unsigned long long>(answer->mapBytes),
                        answer->truncated ? ", truncated" : "");
            return 0;
        }

        if (options.command == "pool-tags")
        {
            const auto answer =
                mpfshost::listPoolTags(options.socketPath, {options.target, options.bigPool, options.tagFilter}, error);
            if (!answer)
            {
                mpfshost::log::error("client", "pool tag listing failed: ", error);
                return 1;
            }

            if (answer->tags.empty())
            {
                std::printf("no matching pool tags\n");
                return 0;
            }

            std::printf("%-4s %10s\n", "TAG", "ENTRIES");
            for (const auto& summary : answer->tags)
            {
                std::printf("%-4s %10u\n", poolTagText(summary.tag).c_str(), static_cast<unsigned>(summary.entries));
            }
            std::printf("%zu of %llu pool tags (%llu entries)%s\n",
                        answer->tags.size(),
                        static_cast<unsigned long long>(answer->mapTags),
                        static_cast<unsigned long long>(answer->mapEntries),
                        answer->truncated ? ", truncated" : "");
            return 0;
        }

        if (options.command == "pfn")
        {
            const auto answer =
                mpfshost::listPfn(options.socketPath, {options.target, options.pfnStart, options.pfnCount}, error);
            if (!answer)
            {
                mpfshost::log::error("client", "pfn listing failed: ", error);
                return 1;
            }

            std::printf("%10s %-9s %-10s %8s %18s %18s %18s %10s %5s %5s %5s  %s\n",
                        "PFN",
                        "TYPE",
                        "EXT",
                        "PID",
                        "VA",
                        "PTE",
                        "ORIG_PTE",
                        "PTEFRAME",
                        "REF",
                        "PRIO",
                        "COLOR",
                        "FLAGS");
            std::size_t next = 0;
            for (std::uint32_t i = 0; i < options.pfnCount; ++i)
            {
                const std::uint32_t pfn = options.pfnStart + i;
                if (next >= answer->entries.size() || answer->entries[next].pfn != pfn)
                {
                    std::printf("%10u %-9s\n", static_cast<unsigned>(pfn), "missing");
                    continue;
                }

                const auto& entry = answer->entries[next++];
                std::printf("%10u %-9s %-10s %8u %18s %18s %18s %10u %5u %5u %5u  %s\n",
                            static_cast<unsigned>(entry.pfn),
                            pfnTypeName(entry.pageLocation),
                            pfnExtendedTypeName(entry.typeExtended),
                            static_cast<unsigned>(entry.pid),
                            hexPointer(entry.va).c_str(),
                            hexPointer(entry.vaPte).c_str(),
                            hexPointer(entry.originalPte).c_str(),
                            static_cast<unsigned>(entry.pteFrame),
                            static_cast<unsigned>(entry.referenceCount),
                            static_cast<unsigned>(entry.priority),
                            static_cast<unsigned>(entry.pageColor),
                            pfnFlagNames(entry.flags).c_str());
            }
            std::printf("%zu of %u pfn entries\n", answer->entries.size(), static_cast<unsigned>(options.pfnCount));
            return 0;
        }

        if (options.command == "vfs-list")
        {
            const auto entries =
                mpfshost::listVfs(options.socketPath, {options.target, options.path, options.nameFilter}, error);
            if (!entries)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            if (entries->empty())
            {
                std::printf("no matching vfs entries under '%s'\n", options.path.c_str());
                return 0;
            }

            std::printf("%-4s %12s  %s\n", "TYPE", "SIZE", "NAME");
            for (const auto& entry : *entries)
            {
                std::printf("%-4s %12llu  %s\n",
                            entry.isDirectory ? "DIR" : "FILE",
                            static_cast<unsigned long long>(entry.size),
                            entry.name.c_str());
            }
            return 0;
        }

        if (options.command == "vfs-read")
        {
            const std::uint32_t readSize = options.sizeSet ? options.size : 4096;
            const auto          bytes =
                mpfshost::readVfs(options.socketPath, {options.target, options.path, options.offset, readSize}, error);
            if (!bytes)
            {
                mpfshost::log::error("client", error);
                return 1;
            }
            if (bytes->empty())
            {
                return 0;
            }

            const auto dump = mpfshost::formatHexDump(*bytes, options.offset);
            std::fputs(dump.c_str(), stdout);
            return 0;
        }

        if (options.command == "vfs-write")
        {
            const auto written = mpfshost::writeVfs(
                options.socketPath, {options.target, options.path, options.offset, options.data}, error);
            if (!written)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            std::printf("wrote %llu bytes\n", static_cast<unsigned long long>(*written));
            return 0;
        }

        if (options.command == "read")
        {
            const auto data = mpfshost::readMemory(
                options.socketPath, {options.target, options.pid, options.address, options.size}, error);
            if (!data)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            const auto dump = mpfshost::formatHexDump(*data, options.address);
            std::fputs(dump.c_str(), stdout);
            return 0;
        }

        if (options.command == "write")
        {
            const auto written = mpfshost::writeMemory(
                options.socketPath, {options.target, options.pid, options.address, options.data}, error);
            if (!written)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            std::printf("wrote %llu bytes\n", static_cast<unsigned long long>(*written));
            return 0;
        }

        if (options.command == "scatter")
        {
            auto session = mpfshost::createScatter(options.socketPath, options.target, options.pid, error);
            if (!session)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            std::vector<mpfshost::ScatterItem> entries;
            entries.reserve(options.ranges.size());
            for (const auto& range : options.ranges)
            {
                entries.push_back({range.address, range.size, false, {}});
            }

            if (!session->prepare(entries, error))
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            auto results = session->execute(error);
            if (!results)
            {
                mpfshost::log::error("client", error);
                return 1;
            }
            printScatterResults(*results);

            for (std::uint32_t round = 1; round < options.repeat; ++round)
            {
                results = session->executeRead(error);
                if (!results)
                {
                    mpfshost::log::error("client", error);
                    return 1;
                }
                printScatterResults(*results);
            }

            if (options.clear && !session->clear(error))
            {
                mpfshost::log::error("client", error);
                return 1;
            }
            if (!session->close(error))
            {
                mpfshost::log::error("client", error);
                return 1;
            }
            return 0;
        }

        if (options.command == "vm-list")
        {
            const auto vms = mpfshost::listVms(options.socketPath, options.target, error);
            if (!vms)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            if (vms->empty())
            {
                std::printf("no VM partitions\n");
                return 0;
            }

            std::printf("%5s %-24s %12s %5s %6s %6s %6s %8s\n",
                        "INDEX",
                        "NAME",
                        "GPAMAX",
                        "KIND",
                        "ACTIVE",
                        "RO",
                        "PHYS",
                        "VMMEM");
            for (std::size_t index = 0; index < vms->size(); ++index)
            {
                const auto& vm = (*vms)[index];
                std::printf("%5zu %-24s 0x%010llx %5u %6s %6s %6s %8u\n",
                            index,
                            vm.name.c_str(),
                            static_cast<unsigned long long>(vm.gpaMax),
                            static_cast<unsigned>(vm.kind),
                            vm.active ? "yes" : "no",
                            vm.readOnly ? "yes" : "no",
                            vm.physicalOnly ? "yes" : "no",
                            static_cast<unsigned>(vm.vmMemPid));
            }
            return 0;
        }

        if (options.command == "phys-read")
        {
            const auto data = mpfshost::readPhysicalMemory(
                options.socketPath, {options.target, options.vmIndex, options.address, options.size}, error);
            if (!data)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            const auto dump = mpfshost::formatHexDump(*data, options.address);
            std::fputs(dump.c_str(), stdout);
            return 0;
        }

        if (options.command == "phys-write")
        {
            const auto written = mpfshost::writePhysicalMemory(
                options.socketPath, {options.target, options.vmIndex, options.address, options.data}, error);
            if (!written)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            std::printf("wrote %llu bytes\n", static_cast<unsigned long long>(*written));
            return 0;
        }

        if (options.command == "vm-translate")
        {
            const auto translated =
                mpfshost::translateGpa(options.socketPath, {options.target, options.vmIndex, options.address}, error);
            if (!translated)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            std::printf("pa=%s va=%s\n",
                        translated->paPresent ? hexPointer(translated->pa).c_str() : "-",
                        translated->vaPresent ? hexPointer(translated->va).c_str() : "-");
            return 0;
        }

        if (options.command == "virt2phys")
        {
            const auto pa =
                mpfshost::virt2Phys(options.socketPath, {options.target, options.pid, options.address}, error);
            if (!pa)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            std::printf("pa=%s\n", hexPointer(*pa).c_str());
            return 0;
        }

        if (options.command == "vquery")
        {
            const auto region =
                mpfshost::queryVirtual(options.socketPath, {options.target, options.pid, options.address}, error);
            if (!region)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            std::printf("base=%s\n", hexPointer(region->baseAddress).c_str());
            std::printf("allocation-base=%s\n", hexPointer(region->allocationBase).c_str());
            std::printf("size=%s\n", hexPointer(region->regionSize).c_str());
            std::printf("allocation-protect=%s (0x%x)\n",
                        protectName(region->allocationProtect).c_str(),
                        static_cast<unsigned>(region->allocationProtect));
            std::printf("state=%s (0x%x)\n", stateName(region->state), static_cast<unsigned>(region->state));
            std::printf(
                "protect=%s (0x%x)\n", protectName(region->protect).c_str(), static_cast<unsigned>(region->protect));
            std::printf("type=%s (0x%x)\n", regionTypeName(region->type), static_cast<unsigned>(region->type));
            std::printf("partition=%u\n", static_cast<unsigned>(region->partitionId));
            return 0;
        }

        if (options.command == "page")
        {
            std::vector<mpfshost::PageRange> ranges;
            ranges.reserve(options.ranges.size());
            for (const auto& range : options.ranges)
            {
                ranges.push_back({range.address, range.size});
            }

            const auto pages =
                mpfshost::readPages(options.socketPath, {options.target, options.pid, std::move(ranges)}, error);
            if (!pages)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            for (const auto& page : *pages)
            {
                std::printf(
                    "page 0x%016llx  %s\n", static_cast<unsigned long long>(page.address), page.ok ? "ok" : "failed");
                if (page.ok)
                {
                    const auto dump = mpfshost::formatHexDump(page.data, page.address);
                    std::fputs(dump.c_str(), stdout);
                }
            }
            std::printf("%zu pages\n", pages->size());
            return 0;
        }

        if (options.command == "prefetch")
        {
            std::vector<mpfshost::PageRange> ranges;
            ranges.reserve(options.ranges.size());
            for (const auto& range : options.ranges)
            {
                ranges.push_back({range.address, range.size});
            }

            const auto count =
                mpfshost::prefetchPages(options.socketPath, {options.target, options.pid, std::move(ranges)}, error);
            if (!count)
            {
                mpfshost::log::error("client", error);
                return 1;
            }
            std::printf("%u pages prefetched\n", static_cast<unsigned>(*count));
            return 0;
        }

        if (options.command == "config-set")
        {
            const auto applied = mpfshost::setConfig(
                options.socketPath, {options.target, options.configOption, options.pid, options.value}, error);
            if (!applied)
            {
                mpfshost::log::error("client", error);
                return 1;
            }
            std::printf("set %s = 0x%llx\n",
                        std::string {mpfshost::configOptionName(options.configOption)}.c_str(),
                        static_cast<unsigned long long>(*applied));
            return 0;
        }

        if (options.command == "debug-status" || options.command == "debug-stop" || options.command == "debug-continue")
        {
            std::optional<mpfshost::DebugStatus> status;
            if (options.command == "debug-status")
            {
                status = mpfshost::debugState(options.socketPath, {options.target}, error);
            }
            else
            {
                const auto action = options.command == "debug-stop" ? mpfshost::DebugControlAction::Stop
                                                                    : mpfshost::DebugControlAction::Continue;
                status            = mpfshost::debugControl(options.socketPath, {options.target}, action, error);
            }
            if (!status)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            printDebugStatus(*status);
            return 0;
        }

        if (options.command == "debug-attach")
        {
            const auto response = mpfshost::debugAttach(
                options.socketPath,
                {options.target},
                [](const mpfshost::DebugAttachResponse& attach)
                {
                    std::printf(
                        "stub-mode=%s  guest-paused=%s\n", attach.stubMode.c_str(), attach.guestPaused ? "yes" : "no");
                    printDebugStatus(attach.status);
                    std::printf("attached; press Enter to detach or Ctrl-C to close the connection\n");
                    int character = 0;
                    while ((character = std::getchar()) != '\n' && character != EOF)
                    {
                    }
                },
                error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }
            return 0;
        }

        if (options.command == "debug-detach")
        {
            const auto response = mpfshost::debugDetach(options.socketPath, {options.target}, error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }
            std::printf("stub-stopped=%s  foreign-stub=%s  guest-resumed=%s  detached=%s\n",
                        response->stubStopped ? "yes" : "no",
                        response->foreignStub ? "yes" : "no",
                        response->guestResumed ? "yes" : "no",
                        response->detached ? "yes" : "no");
            printDebugStatus(response->status);
            return 0;
        }

        if (options.command == "debug-registers")
        {
            const auto records = mpfshost::debugRegisters(
                options.socketPath, {options.target, options.vcpuSet ? options.vcpu : mpfshost::kDebugAllVcpus}, error);
            if (!records)
            {
                mpfshost::log::error("client", error);
                return 1;
            }
            if (records->empty())
            {
                std::printf("no vCPU registers were returned\n");
                return 0;
            }

            for (const auto& record : *records)
            {
                printDebugRecord(record);
            }
            return 0;
        }

        if (options.command == "debug-threads")
        {
            const auto threads = mpfshost::debugThreads(options.socketPath, {options.target}, error);
            if (!threads)
            {
                mpfshost::log::error("client", error);
                return 1;
            }
            if (threads->empty())
            {
                std::printf("no debug threads were returned\n");
                return 0;
            }
            for (const auto& thread : *threads)
            {
                printDebugThread(thread);
            }
            return 0;
        }

        if (options.command == "disassemble")
        {
            const auto response = mpfshost::disassemble(
                options.socketPath, {options.target, options.pid, options.address, options.count}, error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }
            for (const auto& instruction : response->instructions)
            {
                std::printf("0x%016llx  ", static_cast<unsigned long long>(instruction.address));
                std::size_t width = 0;
                for (const auto value : instruction.bytes)
                {
                    width += 3;
                    std::printf("%02x ", std::to_integer<unsigned>(value));
                }
                while (width < 47)
                {
                    std::putchar(' ');
                    ++width;
                }
                std::printf("%s\n", instruction.text.c_str());
            }
            return 0;
        }

        if (options.command == "debug-backtrace")
        {
            const auto response = mpfshost::debugBacktrace(options.socketPath,
                                                           {options.target,
                                                            options.pid,
                                                            options.vcpuSet ? options.vcpu : 0,
                                                            options.threadSet ? options.threadId : std::string {},
                                                            options.maxFrames},
                                                           error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }
            if (response->frames.empty())
            {
                std::printf("no frames were returned\n");
                return 0;
            }

            for (std::size_t index = 0; index < response->frames.size(); ++index)
            {
                const auto& frame = response->frames[index];
                std::printf("#%-2zu  0x%016llx  rbp=0x%016llx\n",
                            index,
                            static_cast<unsigned long long>(frame.address),
                            static_cast<unsigned long long>(frame.framePointer));
                if (!frame.module.empty())
                {
                    std::printf("     module=%s+0x%llx\n",
                                frame.module.c_str(),
                                static_cast<unsigned long long>(frame.moduleOffset));
                }
                if (frame.exportFound)
                {
                    std::printf("     export=%s+0x%llx\n",
                                frame.exportName.empty() ? "?" : frame.exportName.c_str(),
                                static_cast<unsigned long long>(frame.exportOffset));
                }
                if (frame.pdbFound)
                {
                    std::printf("     symbol=%s+0x%x\n", frame.pdbSymbol.c_str(), frame.pdbDisplacement);
                }
            }
            if (response->truncated)
            {
                std::printf("truncated: %s\n",
                            response->stopReason.empty() ? "the frame walk stopped" : response->stopReason.c_str());
            }
            return 0;
        }

        if (options.command == "debug-watch")
        {
            const std::string requestText =
                options.addressSymbolic
                    ? mpfshost::formatSymbolAddress({options.addressModule, options.addressSymbol, options.address})
                    : std::string {};
            const auto end = mpfshost::debugWatch(
                options.socketPath,
                {options.target,
                 options.pid,
                 options.address,
                 options.watchWidth,
                 options.watchAccess,
                 options.maxHits,
                 options.watchTimeoutSeconds * 1000,
                 options.addressModule,
                 options.addressSymbol},
                [&requestText](const mpfshost::DebugWatchData& hit)
                {
                    std::printf("hit %u  vcpu %u  thread %s",
                                static_cast<unsigned>(hit.hitIndex),
                                static_cast<unsigned>(hit.vcpu),
                                hit.threadId.c_str());
                    printGuestContext(hit.guestPid, hit.guestTid, hit.processFound, hit.threadFound);
                    std::printf("\n");
                    if (!requestText.empty())
                    {
                        std::printf("  target=%s  address=0x%016llx\n",
                                    requestText.c_str(),
                                    static_cast<unsigned long long>(hit.address));
                    }
                    std::printf("  address=0x%016llx  old=0x%016llx  new=0x%016llx\n",
                                static_cast<unsigned long long>(hit.address),
                                static_cast<unsigned long long>(hit.oldValue),
                                static_cast<unsigned long long>(hit.newValue));
                    std::printf("  rip=0x%016llx  rsp=0x%016llx  cr3=0x%016llx\n",
                                static_cast<unsigned long long>(hit.rip),
                                static_cast<unsigned long long>(hit.rsp),
                                static_cast<unsigned long long>(hit.cr3));
                    if (!hit.module.empty())
                    {
                        std::printf("  module=%s+0x%llx\n",
                                    hit.module.c_str(),
                                    static_cast<unsigned long long>(hit.moduleOffset));
                    }
                    if (hit.exportFound)
                    {
                        std::printf("  export=%s+0x%llx\n",
                                    hit.exportName.empty() ? "?" : hit.exportName.c_str(),
                                    static_cast<unsigned long long>(hit.exportOffset));
                    }
                    if (hit.pdbFound)
                    {
                        std::printf("  symbol=%s+0x%x\n", hit.pdbSymbol.c_str(), hit.pdbDisplacement);
                    }
                    if (!hit.bytesBefore.empty())
                    {
                        std::printf("  before=");
                        for (const auto value : hit.bytesBefore)
                        {
                            std::printf("%02x", std::to_integer<unsigned>(value));
                        }
                        std::printf("\n");
                        if (const auto store = mpfshost::disasm::decodeStoreBefore(hit.bytesBefore))
                        {
                            std::printf("  store=%s\n", store->text.c_str());
                        }
                    }
                },
                error);
            if (!end)
            {
                mpfshost::log::error("client", error);
                return 1;
            }
            std::printf("%s  hits=%u  skipped=%u\n",
                        std::string {mpfshost::debugWatchStatusName(end->status)}.c_str(),
                        static_cast<unsigned>(end->hitCount),
                        static_cast<unsigned>(end->skippedCount));
            if (!end->error.empty())
            {
                mpfshost::log::error("client", end->error);
            }
            return end->status == mpfshost::DebugWatchStatus::Completed ? 0 : 1;
        }

        if (options.command == "debug-break")
        {
            const std::string requestText =
                options.addressSymbolic
                    ? mpfshost::formatSymbolAddress({options.addressModule, options.addressSymbol, options.address})
                    : std::string {};
            const auto response = mpfshost::debugBreakpoint(options.socketPath,
                                                            {options.target,
                                                             options.pid,
                                                             options.address,
                                                             options.breakpointKind,
                                                             options.watchTimeoutSeconds * 1000,
                                                             options.addressModule,
                                                             options.addressSymbol},
                                                            error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }
            if (response->status == mpfshost::DebugBreakpointStatus::Hit)
            {
                std::printf(
                    "hit  vcpu %u  thread %s", static_cast<unsigned>(response->vcpu), response->threadId.c_str());
                printGuestContext(
                    response->guestPid, response->guestTid, response->processFound, response->threadFound);
                std::printf("\n");
                if (!requestText.empty())
                {
                    std::printf("  target=%s  address=0x%016llx\n",
                                requestText.c_str(),
                                static_cast<unsigned long long>(response->address));
                }
                std::printf("  address=0x%016llx  rip=0x%016llx  rsp=0x%016llx  cr3=0x%016llx\n",
                            static_cast<unsigned long long>(response->address),
                            static_cast<unsigned long long>(response->rip),
                            static_cast<unsigned long long>(response->rsp),
                            static_cast<unsigned long long>(response->cr3));
                if (response->unverified)
                {
                    std::printf("  unverified=yes  the stop CR3 did not match the target process\n");
                }
                if (!response->module.empty())
                {
                    std::printf("  module=%s+0x%llx\n",
                                response->module.c_str(),
                                static_cast<unsigned long long>(response->moduleOffset));
                }
                if (response->exportFound)
                {
                    std::printf("  export=%s+0x%llx\n",
                                response->exportName.empty() ? "?" : response->exportName.c_str(),
                                static_cast<unsigned long long>(response->exportOffset));
                }
                if (response->pdbFound)
                {
                    std::printf("  symbol=%s+0x%x\n", response->pdbSymbol.c_str(), response->pdbDisplacement);
                }
                if (!response->bytesAtRip.empty())
                {
                    std::printf("  bytes=");
                    for (const auto value : response->bytesAtRip)
                    {
                        std::printf("%02x", std::to_integer<unsigned>(value));
                    }
                    std::printf("\n");
                }
            }
            std::printf("%s  skipped=%u\n",
                        std::string {mpfshost::debugBreakpointStatusName(response->status)}.c_str(),
                        static_cast<unsigned>(response->skippedCount));
            if (!response->error.empty())
            {
                mpfshost::log::error("client", response->error);
            }
            return response->status == mpfshost::DebugBreakpointStatus::Hit ? 0 : 1;
        }

        if (options.command == "debug-step")
        {
            const auto response = mpfshost::debugStep(options.socketPath,
                                                      {options.target,
                                                       options.vcpuSet ? options.vcpu : 0,
                                                       options.threadSet ? options.threadId : std::string {}},
                                                      error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }
            printDebugRecord(response->record);
            return 0;
        }

        if (options.command == "debug-arm-break")
        {
            const auto response = mpfshost::debugArmBreakpoint(
                options.socketPath, {options.target, options.pid, options.address, options.breakpointKind}, error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }
            printDebugArmBreakpoint(*response);
            return 0;
        }

        if (options.command == "debug-arm-watch")
        {
            const auto response = mpfshost::debugArmWatch(
                options.socketPath,
                {options.target, options.pid, options.address, options.watchWidth, options.watchAccess},
                error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }
            printDebugArmWatch(*response);
            return 0;
        }

        if (options.command == "debug-remove-break" || options.command == "debug-remove-watch")
        {
            const mpfshost::DebugResourceRemoveQuery query {options.target, options.resourceId};
            const auto response = options.command == "debug-remove-break"
                                    ? mpfshost::debugRemoveBreakpoint(options.socketPath, query, error)
                                    : mpfshost::debugRemoveWatch(options.socketPath, query, error);
            if (!response)
            {
                mpfshost::log::error("client", error);
                return 1;
            }
            std::printf("id=%u  removed=%s\n", static_cast<unsigned>(response->id), response->removed ? "yes" : "no");
            return response->removed ? 0 : 1;
        }

        if (options.command == "debug-breakpoints" || options.command == "debug-watchpoints")
        {
            if (options.command == "debug-breakpoints")
            {
                const auto response = mpfshost::debugListBreakpoints(options.socketPath, {options.target}, error);
                if (!response)
                {
                    mpfshost::log::error("client", error);
                    return 1;
                }
                if (response->empty())
                {
                    std::printf("no breakpoints are armed\n");
                    return 0;
                }
                for (const auto& breakpoint : *response)
                {
                    printDebugArmBreakpoint(breakpoint);
                }
            }
            else
            {
                const auto response = mpfshost::debugListWatches(options.socketPath, {options.target}, error);
                if (!response)
                {
                    mpfshost::log::error("client", error);
                    return 1;
                }
                if (response->empty())
                {
                    std::printf("no watchpoints are armed\n");
                    return 0;
                }
                for (const auto& watch : *response)
                {
                    printDebugArmWatch(watch);
                }
            }
            return 0;
        }

        if (options.command == "debug-resume")
        {
            const auto status = mpfshost::debugContinueArmed(options.socketPath, {options.target}, error);
            if (!status)
            {
                mpfshost::log::error("client", error);
                return 1;
            }
            printDebugStatus(*status);
            return 0;
        }

        if (options.command == "debug-wait")
        {
            const auto end = mpfshost::debugWait(
                options.socketPath,
                {options.target, options.watchTimeoutSeconds * 1000, options.maxHits},
                [](const mpfshost::DebugWaitHit& hit)
                {
                    std::printf("hit  id=%u  %s  address=0x%016llx\n",
                                static_cast<unsigned>(hit.id),
                                hit.kind == mpfshost::DebugResourceKind::Watchpoint ? "watchpoint" : "breakpoint",
                                static_cast<unsigned long long>(hit.address));
                    std::printf(
                        "  vcpu %u  thread %s", static_cast<unsigned>(hit.record.vcpu), hit.record.threadId.c_str());
                    printGuestContext(
                        hit.record.guestPid, hit.record.guestTid, hit.record.processFound, hit.record.threadFound);
                    std::printf("  rip=0x%016llx  rsp=0x%016llx  cr3=0x%016llx\n",
                                static_cast<unsigned long long>(hit.record.rip),
                                static_cast<unsigned long long>(hit.record.rsp),
                                static_cast<unsigned long long>(hit.record.cr3));
                    if (hit.unverified)
                    {
                        std::printf("  unverified=yes  the stop CR3 did not match the target process\n");
                    }
                    if (!hit.module.empty())
                    {
                        std::printf("  module=%s+0x%llx\n",
                                    hit.module.c_str(),
                                    static_cast<unsigned long long>(hit.moduleOffset));
                    }
                    if (hit.exportFound)
                    {
                        std::printf("  export=%s+0x%llx\n",
                                    hit.exportName.empty() ? "?" : hit.exportName.c_str(),
                                    static_cast<unsigned long long>(hit.exportOffset));
                    }
                    if (hit.pdbFound)
                    {
                        std::printf("  symbol=%s+0x%x\n", hit.pdbSymbol.c_str(), hit.pdbDisplacement);
                    }
                },
                error);
            if (!end)
            {
                mpfshost::log::error("client", error);
                return 1;
            }
            std::printf("%s  hits=%u  skipped=%u\n",
                        std::string {mpfshost::debugWatchStatusName(end->status)}.c_str(),
                        static_cast<unsigned>(end->hitCount),
                        static_cast<unsigned>(end->skippedCount));
            if (!end->error.empty())
            {
                mpfshost::log::error("client", end->error);
            }
            return end->status == mpfshost::DebugWatchStatus::Completed ? 0 : 1;
        }

        if (options.command == "search")
        {
            mpfshost::SearchTerm term;
            if (options.hexPattern)
            {
                const auto parsed = mpfshost::parseHexPattern(options.pattern);
                if (!parsed)
                {
                    mpfshost::log::error("client", "--hex-pattern needs 1 to 32 bytes, with ?? for wildcards");
                    return 1;
                }
                term = *parsed;
            }
            else
            {
                term = mpfshost::makeSearchTerm(options.pattern);
            }
            if (term.pattern.size() > mpfshost::kMaxSearchPatternLength)
            {
                mpfshost::log::error(
                    "client", "a search pattern is at most ", mpfshost::kMaxSearchPatternLength, " bytes");
                return 1;
            }
            if (!mpfshost::isSearchableTerm(term))
            {
                // VMMDLL_MemSearch fails on an all-zero pattern, wildcards included, so the host refuses it;
                // catching it here gives the user the reason instead of a MalformedPayload round trip.
                mpfshost::log::error("client", "a search pattern needs at least one non-zero byte");
                return 1;
            }

            const auto result = mpfshost::searchMemory(options.socketPath,
                                                       {options.target,
                                                        options.pid,
                                                        std::move(term),
                                                        options.alignBytes,
                                                        options.minAddress,
                                                        options.maxAddress,
                                                        options.maxResults},
                                                       error);
            if (!result)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            for (const auto address : result->addresses)
            {
                std::printf("0x%016llx\n", static_cast<unsigned long long>(address));
            }
            std::printf("%zu hits%s\n", result->addresses.size(), result->truncated ? " (truncated)" : "");
            return 0;
        }

        if (options.command == "phys-scatter")
        {
            std::vector<mpfshost::ScatterItem> entries;
            entries.reserve(options.ranges.size());
            for (const auto& range : options.ranges)
            {
                entries.push_back({range.address, range.size, false, {}});
            }

            const auto results = mpfshost::physicalScatter(
                options.socketPath, {options.target, options.vmIndex, std::move(entries)}, error);
            if (!results)
            {
                mpfshost::log::error("client", error);
                return 1;
            }

            printScatterResults(*results);
            return 0;
        }

        const auto response = mpfshost::bindTarget(options.socketPath, options.target, error);
        if (!response)
        {
            mpfshost::log::error("client", error);
            return 1;
        }

        std::printf("bound session %llu to %s (pid %u, qmp %s)\n",
                    static_cast<unsigned long long>(response->sessionId),
                    response->target.name.c_str(),
                    static_cast<unsigned>(response->target.qemuPid),
                    response->target.qmpSocketPath.c_str());
        return 0;
    }
} // namespace mpfshost::cli
