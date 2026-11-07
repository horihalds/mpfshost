#include "host/vmm_config.hpp"

#include <cstdint>
#include <optional>

namespace mpfshost
{
    namespace
    {
        std::optional<ULONG64> optionConstant(ConfigOption option)
        {
            switch (option)
            {
            case ConfigOption::TickPeriod:
                return VMMDLL_OPT_CONFIG_TICK_PERIOD;
            case ConfigOption::ReadCacheTicks:
                return VMMDLL_OPT_CONFIG_READCACHE_TICKS;
            case ConfigOption::TlbCacheTicks:
                return VMMDLL_OPT_CONFIG_TLBCACHE_TICKS;
            case ConfigOption::ProcCacheTicksPartial:
                return VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_PARTIAL;
            case ConfigOption::ProcCacheTicksTotal:
                return VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_TOTAL;
            case ConfigOption::RefreshAll:
                return VMMDLL_OPT_REFRESH_ALL;
            case ConfigOption::RefreshFreqMem:
                return VMMDLL_OPT_REFRESH_FREQ_MEM;
            case ConfigOption::RefreshFreqMemPartial:
                return VMMDLL_OPT_REFRESH_FREQ_MEM_PARTIAL;
            case ConfigOption::RefreshFreqTlb:
                return VMMDLL_OPT_REFRESH_FREQ_TLB;
            case ConfigOption::RefreshFreqTlbPartial:
                return VMMDLL_OPT_REFRESH_FREQ_TLB_PARTIAL;
            case ConfigOption::RefreshFreqFast:
                return VMMDLL_OPT_REFRESH_FREQ_FAST;
            case ConfigOption::RefreshFreqMedium:
                return VMMDLL_OPT_REFRESH_FREQ_MEDIUM;
            case ConfigOption::RefreshFreqSlow:
                return VMMDLL_OPT_REFRESH_FREQ_SLOW;
            case ConfigOption::RefreshSpecificHeapAlloc:
                return VMMDLL_OPT_REFRESH_SPECIFIC_HEAP_ALLOC;
            case ConfigOption::RefreshSpecificKobject:
                return VMMDLL_OPT_REFRESH_SPECIFIC_KOBJECT;
            case ConfigOption::RefreshSpecificNet:
                return VMMDLL_OPT_REFRESH_SPECIFIC_NET;
            case ConfigOption::RefreshSpecificPfn:
                return VMMDLL_OPT_REFRESH_SPECIFIC_PFN;
            case ConfigOption::RefreshSpecificPhysmemmap:
                return VMMDLL_OPT_REFRESH_SPECIFIC_PHYSMEMMAP;
            case ConfigOption::RefreshSpecificPool:
                return VMMDLL_OPT_REFRESH_SPECIFIC_POOL;
            case ConfigOption::RefreshSpecificRegistry:
                return VMMDLL_OPT_REFRESH_SPECIFIC_REGISTRY;
            case ConfigOption::RefreshSpecificServices:
                return VMMDLL_OPT_REFRESH_SPECIFIC_SERVICES;
            case ConfigOption::RefreshSpecificThreadcs:
                return VMMDLL_OPT_REFRESH_SPECIFIC_THREADCS;
            case ConfigOption::RefreshSpecificUser:
                return VMMDLL_OPT_REFRESH_SPECIFIC_USER;
            case ConfigOption::RefreshSpecificVm:
                return VMMDLL_OPT_REFRESH_SPECIFIC_VM;
            case ConfigOption::RefreshSpecificProcess:
                return VMMDLL_OPT_REFRESH_SPECIFIC_PROCESS;
            case ConfigOption::ProcessDtb:
                return VMMDLL_OPT_PROCESS_DTB;
            case ConfigOption::ProcessDtbFastLowIntegrity:
                return VMMDLL_OPT_PROCESS_DTB_FAST_LOWINTEGRITY;
            }
            return std::nullopt;
        }
    } // namespace

    bool setVmmConfig(VMM_HANDLE handle, const ConfigSetRequest& request, std::string& error)
    {
        if (handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return false;
        }

        auto constant = optionConstant(request.option);
        if (!constant.has_value())
        {
            error = "unsupported config option";
            return false;
        }

        // Process-scoped options carry the pid in the low dword of fOption.
        if (configOptionUsesPid(request.option))
        {
            *constant |= static_cast<ULONG64>(request.pid);
        }

        if (!VMMDLL_ConfigSet(handle, *constant, request.value))
        {
            error = "VMMDLL_ConfigSet failed";
            return false;
        }
        return true;
    }
} // namespace mpfshost
