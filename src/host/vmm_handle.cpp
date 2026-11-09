#include "host/vmm_handle.hpp"

#include "core/log/log.hpp"
#include "host/vmm_config.hpp"
#include "host/vmm_handles.hpp"
#include "host/vmm_heaps.hpp"
#include "host/vmm_kernel_devices.hpp"
#include "host/vmm_kernel_drivers.hpp"
#include "host/vmm_kernel_objects.hpp"
#include "host/vmm_maps.hpp"
#include "host/vmm_memory.hpp"
#include "host/vmm_module_tables.hpp"
#include "host/vmm_modules.hpp"
#include "host/vmm_network.hpp"
#include "host/vmm_pdb.hpp"
#include "host/vmm_pfn.hpp"
#include "host/vmm_physical.hpp"
#include "host/vmm_pool.hpp"
#include "host/vmm_processes.hpp"
#include "host/vmm_pte.hpp"
#include "host/vmm_scatter.hpp"
#include "host/vmm_search.hpp"
#include "host/vmm_services.hpp"
#include "host/vmm_threads.hpp"
#include "host/vmm_unloaded_modules.hpp"
#include "host/vmm_users.hpp"
#include "host/vmm_vm.hpp"

#include <cstddef>
#include <utility>

namespace mpfshost
{
    VmmHandle::~VmmHandle()
    {
        _debug.shutdown();
        if (_handle != nullptr)
        {
            log::debug("host", "released VMM handle for target ", _targetName);
            VMMDLL_Close(_handle);
        }
    }

    std::optional<std::vector<ProcessDescriptor>> VmmHandle::listProcesses(const ProcessFilter& filter,
                                                                           std::string&         error)
    {
        auto processes = listVmmProcesses(_handle, error);
        if (!processes.has_value())
        {
            return std::nullopt;
        }

        std::vector<ProcessDescriptor> matches;
        matches.reserve(processes->size());
        for (auto& process : *processes)
        {
            if (matchesProcessFilter(filter, process))
            {
                matches.push_back(std::move(process));
            }
        }
        return matches;
    }

    std::optional<std::vector<ProcessSummary>> VmmHandle::listAllProcesses(const ProcessFilter& filter,
                                                                           std::string&         error)
    {
        auto processes = listAllVmmProcesses(_handle, error);
        if (!processes.has_value())
        {
            return std::nullopt;
        }

        std::vector<ProcessSummary> matches;
        matches.reserve(processes->size());
        for (auto& process : *processes)
        {
            if (matchesProcessFilter(filter, process.process))
            {
                matches.push_back(std::move(process));
            }
        }
        return matches;
    }

    std::optional<ProcessInfoResponse>
    VmmHandle::processInfo(const ProcessInfoRequest& request, LookupStatus& status, std::string& error)
    {
        return queryVmmProcess(_handle, request, status, error);
    }

    std::optional<ProcessByDtbResponse>
    VmmHandle::processByDtb(const ProcessByDtbRequest& request, LookupStatus& status, std::string& error)
    {
        return _lookupCache.processByDtb(request, status, error);
    }

    std::optional<std::vector<ModuleDescriptor>> VmmHandle::listModules(const ModuleListRequest& request,
                                                                        std::string&             error)
    {
        auto modules = listVmmModules(_handle, request, error);
        if (!modules.has_value())
        {
            return std::nullopt;
        }

        std::vector<ModuleDescriptor> matches;
        matches.reserve(modules->size());
        for (auto& module : *modules)
        {
            if (matchesModuleFilter(request, module))
            {
                matches.push_back(std::move(module));
            }
        }
        return matches;
    }

    std::optional<SymbolLookupResponse>
    VmmHandle::lookupSymbol(const SymbolLookupRequest& request, LookupStatus& status, std::string& error)
    {
        return lookupVmmSymbol(_handle, request, status, error);
    }

    std::optional<ModuleLayoutResponse>
    VmmHandle::moduleLayout(const ModuleLayoutRequest& request, LookupStatus& status, std::string& error)
    {
        return queryVmmModuleLayout(_handle, request, status, error);
    }

    std::optional<ModuleByAddressResponse>
    VmmHandle::moduleByAddress(const ModuleByAddressRequest& request, LookupStatus& status, std::string& error)
    {
        return _lookupCache.moduleByAddress(request, status, error);
    }

    std::optional<PdbLoadResponse>
    VmmHandle::pdbLoad(const PdbLoadRequest& request, LookupStatus& status, std::string& error)
    {
        return loadVmmPdb(_handle, request, status, error);
    }

    std::optional<PdbSymbolNameResponse>
    VmmHandle::pdbSymbolName(const PdbSymbolNameRequest& request, LookupStatus& status, std::string& error)
    {
        return lookupVmmPdbSymbolName(_handle, request, status, error);
    }

    std::optional<PdbSymbolAddressResponse>
    VmmHandle::pdbSymbolAddress(const PdbSymbolAddressRequest& request, LookupStatus& status, std::string& error)
    {
        return lookupVmmPdbSymbolAddress(_handle, request, status, error);
    }

    std::optional<PdbTypeSizeResponse>
    VmmHandle::pdbTypeSize(const PdbTypeSizeRequest& request, LookupStatus& status, std::string& error)
    {
        return lookupVmmPdbTypeSize(_handle, request, status, error);
    }

    std::optional<PdbTypeChildOffsetResponse>
    VmmHandle::pdbTypeChildOffset(const PdbTypeChildOffsetRequest& request, LookupStatus& status, std::string& error)
    {
        return lookupVmmPdbTypeChildOffset(_handle, request, status, error);
    }

    std::optional<std::vector<MemoryMapRegion>> VmmHandle::listMemoryMap(const MemoryMapRequest& request,
                                                                         std::string&            error)
    {
        return listVmmMemoryMap(_handle, request, error);
    }

    std::optional<std::vector<VadExDescriptor>>
    VmmHandle::listVadEx(const VadExListRequest& request, LookupStatus& status, std::string& error)
    {
        return listVmmVadEx(_handle, request, status, error);
    }

    std::optional<std::vector<ThreadDescriptor>> VmmHandle::listThreads(const ThreadListRequest& request,
                                                                        std::string&             error)
    {
        auto threads = listVmmThreads(_handle, request, error);
        if (!threads.has_value())
        {
            return std::nullopt;
        }

        std::vector<ThreadDescriptor> matches;
        matches.reserve(threads->size());
        for (auto& thread : *threads)
        {
            if (matchesThreadFilter(request, thread))
            {
                matches.push_back(std::move(thread));
            }
        }
        return matches;
    }

    std::optional<HandleListResult>
    VmmHandle::listHandles(const HandleListRequest& request, LookupStatus& status, std::string& error)
    {
        return listVmmHandles(_handle, request, status, error);
    }

    std::optional<ExportListResult>
    VmmHandle::listExports(const ExportListRequest& request, LookupStatus& status, std::string& error)
    {
        return listVmmExports(_handle, request, status, error);
    }

    std::optional<ImportListResult>
    VmmHandle::listImports(const ImportListRequest& request, LookupStatus& status, std::string& error)
    {
        return listVmmImports(_handle, request, status, error);
    }

    std::optional<UnloadedModuleListResult>
    VmmHandle::listUnloadedModules(const UnloadedModuleListRequest& request, LookupStatus& status, std::string& error)
    {
        return listVmmUnloadedModules(_handle, request, status, error);
    }

    std::optional<ServiceListResult>
    VmmHandle::listServices(const ServiceListRequest& request, LookupStatus& status, std::string& error)
    {
        return listVmmServices(_handle, request, status, error);
    }

    std::optional<UserListResult>
    VmmHandle::listUsers(const UserListRequest& request, LookupStatus& status, std::string& error)
    {
        return listVmmUsers(_handle, request, status, error);
    }

    std::optional<NetworkListResult>
    VmmHandle::listNetwork(const NetworkListRequest& request, LookupStatus& status, std::string& error)
    {
        return listVmmNetwork(_handle, request, status, error);
    }

    std::optional<HeapListResult>
    VmmHandle::listHeaps(const HeapListRequest& request, LookupStatus& status, std::string& error)
    {
        return listVmmHeaps(_handle, request, status, error);
    }

    std::optional<HeapAllocListResult>
    VmmHandle::listHeapAllocations(const HeapAllocListRequest& request, LookupStatus& status, std::string& error)
    {
        return listVmmHeapAllocations(_handle, request, status, error);
    }

    std::optional<KernelDriverListResult>
    VmmHandle::listKernelDrivers(const KDriverListRequest& request, LookupStatus& status, std::string& error)
    {
        return listVmmKernelDrivers(_handle, request, status, error);
    }

    std::optional<KernelDeviceListResult>
    VmmHandle::listKernelDevices(const KDeviceListRequest& request, LookupStatus& status, std::string& error)
    {
        return listVmmKernelDevices(_handle, request, status, error);
    }

    std::optional<KernelObjectListResult>
    VmmHandle::listKernelObjects(const KObjectListRequest& request, LookupStatus& status, std::string& error)
    {
        return listVmmKernelObjects(_handle, request, status, error);
    }

    std::optional<PteListResult>
    VmmHandle::listPte(const PteListRequest& request, LookupStatus& status, std::string& error)
    {
        return listVmmPte(_handle, request, status, error);
    }

    std::optional<PoolListResult>
    VmmHandle::listPool(const PoolListRequest& request, LookupStatus& status, std::string& error)
    {
        return listVmmPool(_handle, request, status, error);
    }

    std::optional<PoolTagListResult>
    VmmHandle::listPoolTags(const PoolTagListRequest& request, LookupStatus& status, std::string& error)
    {
        return listVmmPoolTags(_handle, request, status, error);
    }

    std::optional<PfnListResult>
    VmmHandle::listPfn(const PfnListRequest& request, LookupStatus& status, std::string& error)
    {
        return listVmmPfn(_handle, request, status, error);
    }

    std::optional<VirtualRegion>
    VmmHandle::queryVirtual(const VirtualQueryRequest& request, LookupStatus& status, std::string& error)
    {
        return queryVmmVirtual(_handle, request, status, error);
    }

    bool VmmHandle::readPages(std::uint32_t                  pid,
                              std::span<const std::uint64_t> addresses,
                              std::vector<PageReadItem>&     out,
                              std::string&                   error)
    {
        return readVmmPages(_handle, pid, addresses, out, error);
    }

    bool VmmHandle::prefetchPages(std::uint32_t pid, std::span<const std::uint64_t> addresses, std::string& error)
    {
        return prefetchVmmPages(_handle, pid, addresses, error);
    }

    bool VmmHandle::searchMemory(const MemorySearchRequest&  request,
                                 std::vector<std::uint64_t>& hits,
                                 bool&                       truncated,
                                 std::string&                error)
    {
        return searchVmmMemory(_handle, request, hits, truncated, error);
    }

    bool VmmHandle::readMemory(
        std::uint32_t pid, std::uint64_t address, std::uint32_t size, std::vector<std::byte>& out, std::string& error)
    {
        out.resize(size);
        return readVmmMemory(_handle, pid, address, std::span<std::byte> {out.data(), out.size()}, error);
    }

    bool VmmHandle::readMemoryPartial(
        std::uint32_t pid, std::uint64_t address, std::uint32_t size, std::vector<std::byte>& out, std::string& error)
    {
        return readVmmMemoryPartial(_handle, pid, address, size, out, error);
    }

    bool VmmHandle::writeMemory(std::uint32_t              pid,
                                std::uint64_t              address,
                                std::span<const std::byte> data,
                                std::string&               error)
    {
        return writeVmmMemory(_handle, pid, address, data, error);
    }

    bool VmmHandle::virt2Phys(std::uint32_t pid, std::uint64_t va, std::uint64_t& pa, std::string& error)
    {
        return virt2PhysAddress(_handle, pid, va, pa, error);
    }

    std::optional<std::vector<VmDescriptor>> VmmHandle::listVms(std::string& error)
    {
        return listVmmVms(_handle, error);
    }

    bool VmmHandle::readPhysical(
        std::uint32_t vmIndex, std::uint64_t gpa, std::uint32_t size, std::vector<std::byte>& out, std::string& error)
    {
        out.assign(size, std::byte {});
        return readVmMemory(_handle, vmIndex, gpa, out, error);
    }

    bool VmmHandle::writePhysical(std::uint32_t              vmIndex,
                                  std::uint64_t              gpa,
                                  std::span<const std::byte> data,
                                  std::string&               error)
    {
        return writeVmMemory(_handle, vmIndex, gpa, data, error);
    }

    bool
    VmmHandle::translateGpa(std::uint32_t vmIndex, std::uint64_t gpa, GpaTranslateResponse& out, std::string& error)
    {
        return translateVmGpa(_handle, vmIndex, gpa, out, error);
    }

    bool VmmHandle::scatterPhysical(std::uint32_t                    vmIndex,
                                    const std::vector<ScatterEntry>& entries,
                                    std::vector<ScatterResult>&      results,
                                    std::string&                     error)
    {
        return scatterVmMemory(_handle, vmIndex, entries, results, error);
    }

    std::shared_ptr<ScatterHandle> VmmHandle::createScatter(std::uint32_t pid, std::string& error)
    {
        if (_handle == nullptr)
        {
            error = "VM handle is closed";
            return nullptr;
        }

        auto scatter = std::make_shared<VmmScatter>(_handle, pid);
        if (!scatter->valid())
        {
            error = "VMMDLL_Scatter_Initialize failed";
            return nullptr;
        }
        return scatter;
    }

    bool VmmHandle::setConfig(const ConfigSetRequest& request, std::string& error)
    {
        return setVmmConfig(_handle, request, error);
    }
} // namespace mpfshost
