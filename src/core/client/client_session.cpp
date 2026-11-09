#include "mpfshost/client.hpp"

#include "core/client/client_legacy.hpp"
#include "core/client/client_transport.hpp"

#include <algorithm>
#include <utility>

namespace mpfshost
{
    namespace legacy
    {
        using ::mpfshost::createScatter;
        using ::mpfshost::debugArmBreakpoint;
        using ::mpfshost::debugArmWatch;
        using ::mpfshost::debugAttach;
        using ::mpfshost::debugBacktrace;
        using ::mpfshost::debugBreakpoint;
        using ::mpfshost::debugContinueArmed;
        using ::mpfshost::debugControl;
        using ::mpfshost::debugDetach;
        using ::mpfshost::debugListBreakpoints;
        using ::mpfshost::debugListWatches;
        using ::mpfshost::debugRegisters;
        using ::mpfshost::debugRemoveBreakpoint;
        using ::mpfshost::debugRemoveWatch;
        using ::mpfshost::debugState;
        using ::mpfshost::debugStep;
        using ::mpfshost::debugThreads;
        using ::mpfshost::debugWait;
        using ::mpfshost::debugWatch;
        using ::mpfshost::disassemble;
        using ::mpfshost::listAllProcesses;
        using ::mpfshost::listExports;
        using ::mpfshost::listHandles;
        using ::mpfshost::listHeapAllocations;
        using ::mpfshost::listHeaps;
        using ::mpfshost::listImports;
        using ::mpfshost::listKernelDevices;
        using ::mpfshost::listKernelDrivers;
        using ::mpfshost::listKernelObjects;
        using ::mpfshost::listMemoryMap;
        using ::mpfshost::listModules;
        using ::mpfshost::listNetwork;
        using ::mpfshost::listPfn;
        using ::mpfshost::listPool;
        using ::mpfshost::listPoolTags;
        using ::mpfshost::listProcesses;
        using ::mpfshost::listPte;
        using ::mpfshost::listServices;
        using ::mpfshost::listThreads;
        using ::mpfshost::listUnloadedModules;
        using ::mpfshost::listUsers;
        using ::mpfshost::listVadEx;
        using ::mpfshost::listVfs;
        using ::mpfshost::listVms;
        using ::mpfshost::lookupSymbol;
        using ::mpfshost::moduleByAddress;
        using ::mpfshost::moduleLayout;
        using ::mpfshost::pdbLoad;
        using ::mpfshost::pdbSymbolAddress;
        using ::mpfshost::pdbSymbolName;
        using ::mpfshost::pdbTypeChildOffset;
        using ::mpfshost::pdbTypeSize;
        using ::mpfshost::physicalScatter;
        using ::mpfshost::prefetchPages;
        using ::mpfshost::processByDtb;
        using ::mpfshost::processInfo;
        using ::mpfshost::queryVirtual;
        using ::mpfshost::readMemory;
        using ::mpfshost::readPages;
        using ::mpfshost::readPhysicalMemory;
        using ::mpfshost::readVfs;
        using ::mpfshost::resolveAddress;
        using ::mpfshost::scatterRead;
        using ::mpfshost::searchMemory;
        using ::mpfshost::setConfig;
        using ::mpfshost::translateGpa;
        using ::mpfshost::virt2Phys;
        using ::mpfshost::writeMemory;
        using ::mpfshost::writePhysicalMemory;
        using ::mpfshost::writeVfs;
    } // namespace legacy

    namespace detail
    {
        struct Connection
        {
            FileDescriptor fd;
            std::string    targetName;
        };
    } // namespace detail

    namespace
    {
        Error makeError(const std::string& text)
        {
            auto code = detail::lastErrorCode();
            if (code == ErrorCode::Internal && text.rfind("server returned", 0) == 0)
            {
                code = ErrorCode::ProtocolError;
            }
            return {code, text.empty() ? "request failed" : text};
        }

        Error invalidArgument(std::string message)
        {
            return {ErrorCode::InvalidArgument, std::move(message)};
        }

        template<class T, class F>
        Result<T> runBound(int fd, const std::string& targetName, F&& operation)
        {
            std::string error;
            detail::resetLastErrorCode();
            detail::ActiveConnectionScope scope {fd, targetName};
            auto                          value = operation(error);
            if (!value)
            {
                return std::unexpected(makeError(error));
            }
            return std::move(*value);
        }

        template<class F>
        Result<void> runBoundVoid(int fd, const std::string& targetName, F&& operation)
        {
            std::string error;
            detail::resetLastErrorCode();
            detail::ActiveConnectionScope scope {fd, targetName};
            if (!operation(error))
            {
                return std::unexpected(makeError(error));
            }
            return {};
        }

        template<class T, class F>
        Result<T> runDirect(F&& operation)
        {
            std::string error;
            detail::resetLastErrorCode();
            auto value = operation(error);
            if (!value)
            {
                return std::unexpected(makeError(error));
            }
            return std::move(*value);
        }

        template<class F>
        Result<void> runDirectVoid(F&& operation)
        {
            std::string error;
            detail::resetLastErrorCode();
            if (!operation(error))
            {
                return std::unexpected(makeError(error));
            }
            return {};
        }

        template<class T>
        List<T> listOf(std::vector<T> items, bool truncated = false)
        {
            return List<T> {std::move(items), truncated};
        }
    } // namespace

    bool Capabilities::supports(MessageType type) const noexcept
    {
        const auto wanted = static_cast<std::uint16_t>(type);
        for (const auto value : requestTypes)
        {
            if (value == wanted)
            {
                return true;
            }
        }
        return false;
    }

    Client::Client(std::filesystem::path socketPath) : _socketPath {std::move(socketPath)} {}

    const std::filesystem::path& Client::socketPath() const noexcept
    {
        return _socketPath;
    }

    Result<Capabilities> Client::capabilities() const
    {
        detail::resetLastErrorCode();
        std::string error;
        const auto  response = getCapabilities(_socketPath, error);
        if (!response)
        {
            return std::unexpected(makeError(error));
        }

        Capabilities capabilities;
        capabilities.serverVersion = response->serverVersion;
        capabilities.requestTypes  = response->requestTypes;
        return capabilities;
    }

    Result<List<Target>> Client::targets() const
    {
        detail::resetLastErrorCode();
        std::string error;
        const auto  response = listTargets(_socketPath, error);
        if (!response)
        {
            return std::unexpected(makeError(error));
        }
        return listOf(std::move(response->targets));
    }

    Result<ClientSession> Client::bind(std::string targetName) const
    {
        detail::resetLastErrorCode();
        std::string error;
        const int   fd = detail::connectToServer(_socketPath, error);
        if (fd < 0)
        {
            return std::unexpected(makeError(error));
        }
        detail::FileDescriptor descriptor {fd};

        const auto response = detail::exchange(fd, MessageType::BindRequest, encodeBindRequest({targetName}), error);
        if (!response)
        {
            return std::unexpected(makeError(error));
        }

        const auto payload = detail::expectResponse(*response, MessageType::BindResponse, error);
        if (!payload)
        {
            return std::unexpected(makeError(error));
        }

        const auto decoded = decodeBindResponse(*payload);
        if (!decoded)
        {
            return std::unexpected(Error {ErrorCode::ProtocolError, "server returned a malformed bind response"});
        }

        auto connection =
            std::make_shared<detail::Connection>(detail::Connection {std::move(descriptor), std::move(targetName)});
        return ClientSession {std::move(connection), decoded->target};
    }

    ClientSession::ClientSession(std::shared_ptr<detail::Connection> connection, Target target)
        : _connection {std::move(connection)}, _target {std::move(target)}
    {
    }

    ClientSession::ClientSession(ClientSession&& other) noexcept
        : _connection {std::move(other._connection)}, _target {std::move(other._target)}
    {
    }

    ClientSession& ClientSession::operator=(ClientSession&& other) noexcept
    {
        if (this != &other)
        {
            _connection = std::move(other._connection);
            _target     = std::move(other._target);
        }
        return *this;
    }

    ClientSession::~ClientSession() = default;

    const Target& ClientSession::target() const noexcept
    {
        return _target;
    }

    const std::string& ClientSession::targetName() const noexcept
    {
        return _target.name;
    }

    int ClientSession::fd() const noexcept
    {
        return _connection ? _connection->fd.fd() : -1;
    }

    void ClientSession::close() noexcept
    {
        _connection.reset();
    }

    Result<List<ProcessDescriptor>> ClientSession::processes(const ProcessFilter& filter)
    {
        auto response = runBound<ProcessListResponse>(fd(),
                                                      targetName(),
                                                      [&](std::string& error)
                                                      {
                                                          return legacy::listProcesses(
                                                              {}, {targetName(), filter.pid, filter.nameFilter}, error);
                                                      });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(response->processes));
    }

    Result<List<ProcessSummary>> ClientSession::allProcesses(const ProcessFilter& filter)
    {
        auto response = runBound<ProcessListAllResponse>(
            fd(),
            targetName(),
            [&](std::string& error)
            {
                return legacy::listAllProcesses({}, {targetName(), filter.pid, filter.nameFilter}, error);
            });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(response->processes));
    }

    Result<ProcessInfoResponse> ClientSession::processInfo(std::uint32_t pid)
    {
        return runBound<ProcessInfoResponse>(fd(),
                                             targetName(),
                                             [&](std::string& error)
                                             {
                                                 return legacy::processInfo({}, {targetName(), pid, {}}, error);
                                             });
    }

    Result<ProcessInfoResponse> ClientSession::processInfo(std::string_view name)
    {
        const std::string text {name};
        return runBound<ProcessInfoResponse>(fd(),
                                             targetName(),
                                             [&](std::string& error)
                                             {
                                                 return legacy::processInfo({}, {targetName(), 0, text}, error);
                                             });
    }

    Result<ProcessByDtbResponse> ClientSession::processByDtb(std::uint64_t dtb)
    {
        return runBound<ProcessByDtbResponse>(fd(),
                                              targetName(),
                                              [&](std::string& error)
                                              {
                                                  return legacy::processByDtb({}, {targetName(), dtb}, error);
                                              });
    }

    Result<List<ModuleDescriptor>>
    ClientSession::modules(std::uint32_t pid, std::string_view nameFilter, std::uint16_t flags)
    {
        const std::string filter {nameFilter};
        auto              response = runBound<std::vector<ModuleDescriptor>>(fd(),
                                                                             targetName(),
                                                                             [&](std::string& error)
                                                                             {
                                                                    return legacy::listModules(
                                                                        {}, {targetName(), pid, filter, flags}, error);
                                                                             });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(*response));
    }

    Result<SymbolLookupResponse>
    ClientSession::lookupSymbol(std::uint32_t pid, std::string_view module, std::string_view symbol)
    {
        const std::string moduleText {module};
        const std::string symbolText {symbol};
        return runBound<SymbolLookupResponse>(fd(),
                                              targetName(),
                                              [&](std::string& error)
                                              {
                                                  return legacy::lookupSymbol(
                                                      {}, {targetName(), pid, moduleText, symbolText}, error);
                                              });
    }

    Result<PdbLoadResponse> ClientSession::pdbLoad(std::uint32_t pid, std::uint64_t moduleBase)
    {
        return runBound<PdbLoadResponse>(fd(),
                                         targetName(),
                                         [&](std::string& error)
                                         {
                                             return legacy::pdbLoad({}, {targetName(), pid, moduleBase}, error);
                                         });
    }

    Result<PdbSymbolNameResponse> ClientSession::pdbSymbolName(std::string_view module, std::uint64_t address)
    {
        const std::string moduleText {module};
        return runBound<PdbSymbolNameResponse>(fd(),
                                               targetName(),
                                               [&](std::string& error)
                                               {
                                                   return legacy::pdbSymbolName(
                                                       {}, {targetName(), moduleText, address}, error);
                                               });
    }

    Result<PdbSymbolAddressResponse> ClientSession::pdbSymbolAddress(std::string_view module, std::string_view symbol)
    {
        const std::string moduleText {module};
        const std::string symbolText {symbol};
        return runBound<PdbSymbolAddressResponse>(fd(),
                                                  targetName(),
                                                  [&](std::string& error)
                                                  {
                                                      return legacy::pdbSymbolAddress(
                                                          {}, {targetName(), moduleText, symbolText}, error);
                                                  });
    }

    Result<PdbTypeSizeResponse> ClientSession::pdbTypeSize(std::string_view module, std::string_view type)
    {
        const std::string moduleText {module};
        const std::string typeText {type};
        return runBound<PdbTypeSizeResponse>(fd(),
                                             targetName(),
                                             [&](std::string& error)
                                             {
                                                 return legacy::pdbTypeSize(
                                                     {}, {targetName(), moduleText, typeText}, error);
                                             });
    }

    Result<PdbTypeChildOffsetResponse>
    ClientSession::pdbTypeChildOffset(std::string_view module, std::string_view type, std::string_view child)
    {
        const std::string moduleText {module};
        const std::string typeText {type};
        const std::string childText {child};
        return runBound<PdbTypeChildOffsetResponse>(fd(),
                                                    targetName(),
                                                    [&](std::string& error)
                                                    {
                                                        return legacy::pdbTypeChildOffset(
                                                            {}, {targetName(), moduleText, typeText, childText}, error);
                                                    });
    }

    Result<ModuleLayoutResponse> ClientSession::moduleLayout(std::uint32_t pid, std::string_view module)
    {
        const std::string moduleText {module};
        return runBound<ModuleLayoutResponse>(fd(),
                                              targetName(),
                                              [&](std::string& error)
                                              {
                                                  return legacy::moduleLayout(
                                                      {}, {targetName(), pid, moduleText}, error);
                                              });
    }

    Result<ModuleByAddressResponse> ClientSession::moduleByAddress(std::uint32_t pid, std::uint64_t address)
    {
        return runBound<ModuleByAddressResponse>(fd(),
                                                 targetName(),
                                                 [&](std::string& error)
                                                 {
                                                     return legacy::moduleByAddress(
                                                         {}, {targetName(), pid, address}, error);
                                                 });
    }

    Result<AddressResolution> ClientSession::resolveAddress(std::uint32_t pid, std::uint64_t address)
    {
        return runBound<AddressResolution>(fd(),
                                           targetName(),
                                           [&](std::string& error)
                                           {
                                               return legacy::resolveAddress({}, {targetName(), pid, address}, error);
                                           });
    }

    Result<List<MemoryMapRegion>> ClientSession::memoryMap(std::uint32_t pid, bool identifyModules)
    {
        auto response = runBound<std::vector<MemoryMapRegion>>(fd(),
                                                               targetName(),
                                                               [&](std::string& error)
                                                               {
                                                                   return legacy::listMemoryMap(
                                                                       {}, {targetName(), pid, identifyModules}, error);
                                                               });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(*response));
    }

    Result<List<VadExDescriptor>>
    ClientSession::vadEx(std::uint32_t pid, std::uint32_t pageOffset, std::uint32_t pageCount)
    {
        auto response = runBound<std::vector<VadExDescriptor>>(
            fd(),
            targetName(),
            [&](std::string& error)
            {
                return legacy::listVadEx({}, {targetName(), pid, pageOffset, pageCount}, error);
            });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(*response));
    }

    Result<List<ThreadDescriptor>> ClientSession::threads(std::uint32_t pid, std::uint32_t tid, std::uint16_t flags)
    {
        auto response = runBound<std::vector<ThreadDescriptor>>(fd(),
                                                                targetName(),
                                                                [&](std::string& error)
                                                                {
                                                                    return legacy::listThreads(
                                                                        {}, {targetName(), pid, tid, flags}, error);
                                                                });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(*response));
    }

    Result<List<HandleDescriptor>> ClientSession::handles(std::uint32_t pid, std::string_view filter)
    {
        const std::string filterText {filter};
        auto              response = runBound<std::vector<HandleDescriptor>>(fd(),
                                                                             targetName(),
                                                                             [&](std::string& error)
                                                                             {
                                                                    return legacy::listHandles(
                                                                        {}, {targetName(), pid, filterText}, error);
                                                                             });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(*response));
    }

    Result<List<ExportDescriptor>> ClientSession::exports(std::uint32_t pid, std::string_view module)
    {
        const std::string moduleText {module};
        auto              response = runBound<std::vector<ExportDescriptor>>(fd(),
                                                                             targetName(),
                                                                             [&](std::string& error)
                                                                             {
                                                                    return legacy::listExports(
                                                                        {}, {targetName(), pid, moduleText}, error);
                                                                             });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(*response));
    }

    Result<List<ImportDescriptor>> ClientSession::imports(std::uint32_t pid, std::string_view module)
    {
        const std::string moduleText {module};
        auto              response = runBound<std::vector<ImportDescriptor>>(fd(),
                                                                             targetName(),
                                                                             [&](std::string& error)
                                                                             {
                                                                    return legacy::listImports(
                                                                        {}, {targetName(), pid, moduleText}, error);
                                                                             });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(*response));
    }

    Result<List<UnloadedModuleDescriptor>> ClientSession::unloadedModules(std::uint32_t pid, std::string_view filter)
    {
        const std::string filterText {filter};
        auto              response = runBound<std::vector<UnloadedModuleDescriptor>>(
            fd(),
            targetName(),
            [&](std::string& error)
            {
                return legacy::listUnloadedModules({}, {targetName(), pid, filterText}, error);
            });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(*response));
    }

    Result<List<ServiceDescriptor>> ClientSession::services(std::string_view filter)
    {
        const std::string filterText {filter};
        auto              response =
            runBound<ServiceListAnswer>(fd(),
                                        targetName(),
                                        [&](std::string& error)
                                        {
                                            return legacy::listServices({}, {targetName(), filterText}, error);
                                        });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(response->services), response->truncated);
    }

    Result<List<UserDescriptor>> ClientSession::users(std::string_view filter)
    {
        const std::string filterText {filter};
        auto              response = runBound<UserListAnswer>(fd(),
                                                              targetName(),
                                                              [&](std::string& error)
                                                              {
                                                     return legacy::listUsers({}, {targetName(), filterText}, error);
                                                              });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(response->users), response->truncated);
    }

    Result<List<NetworkEntryDescriptor>> ClientSession::network(std::uint32_t pid, std::string_view filter)
    {
        const std::string filterText {filter};
        auto              response =
            runBound<NetworkListAnswer>(fd(),
                                        targetName(),
                                        [&](std::string& error)
                                        {
                                            return legacy::listNetwork({}, {targetName(), pid, filterText}, error);
                                        });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(response->entries), response->truncated);
    }

    Result<List<HeapDescriptor>> ClientSession::heaps(std::uint32_t pid)
    {
        auto response = runBound<HeapListAnswer>(fd(),
                                                 targetName(),
                                                 [&](std::string& error)
                                                 {
                                                     return legacy::listHeaps({}, {targetName(), pid}, error);
                                                 });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(response->heaps), response->truncated);
    }

    Result<List<HeapAllocDescriptor>> ClientSession::heapAllocations(std::uint32_t pid, std::uint64_t heap)
    {
        auto response =
            runBound<HeapAllocListAnswer>(fd(),
                                          targetName(),
                                          [&](std::string& error)
                                          {
                                              return legacy::listHeapAllocations({}, {targetName(), pid, heap}, error);
                                          });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(response->allocations), response->truncated);
    }

    Result<List<KernelDriverDescriptor>> ClientSession::kernelDrivers(std::string_view filter)
    {
        const std::string filterText {filter};
        auto              response =
            runBound<KernelDriverAnswer>(fd(),
                                         targetName(),
                                         [&](std::string& error)
                                         {
                                             return legacy::listKernelDrivers({}, {targetName(), filterText}, error);
                                         });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(response->drivers), response->truncated);
    }

    Result<List<KernelDeviceDescriptor>> ClientSession::kernelDevices(std::string_view filter)
    {
        const std::string filterText {filter};
        auto              response =
            runBound<KernelDeviceAnswer>(fd(),
                                         targetName(),
                                         [&](std::string& error)
                                         {
                                             return legacy::listKernelDevices({}, {targetName(), filterText}, error);
                                         });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(response->devices), response->truncated);
    }

    Result<List<KernelObjectDescriptor>> ClientSession::kernelObjects(std::string_view filter)
    {
        const std::string filterText {filter};
        auto              response =
            runBound<KernelObjectAnswer>(fd(),
                                         targetName(),
                                         [&](std::string& error)
                                         {
                                             return legacy::listKernelObjects({}, {targetName(), filterText}, error);
                                         });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(response->objects), response->truncated);
    }

    Result<ClientSession::PteResult> ClientSession::pte(std::uint32_t    pid,
                                                        bool             identifyModules,
                                                        std::string_view filter,
                                                        std::uint64_t    minAddress,
                                                        std::uint64_t    maxAddress)
    {
        const std::string filterText {filter};
        auto              response = runBound<PteAnswer>(
            fd(),
            targetName(),
            [&](std::string& error)
            {
                return legacy::listPte(
                    {}, {targetName(), pid, identifyModules, filterText, minAddress, maxAddress}, error);
            });
        if (!response)
        {
            return std::unexpected(response.error());
        }

        PteResult result;
        result.regions    = listOf(std::move(response->regions), response->truncated);
        result.mapEntries = response->mapEntries;
        return result;
    }

    Result<ClientSession::PoolResult> ClientSession::pool(bool             big,
                                                          std::string_view tagFilter,
                                                          std::uint32_t    minBytes,
                                                          std::uint64_t    minAddress,
                                                          std::uint64_t    maxAddress)
    {
        const std::string filterText {tagFilter};
        auto              response = runBound<PoolAnswer>(
            fd(),
            targetName(),
            [&](std::string& error)
            {
                return legacy::listPool({}, {targetName(), big, filterText, minBytes, minAddress, maxAddress}, error);
            });
        if (!response)
        {
            return std::unexpected(response.error());
        }

        PoolResult result;
        result.entries    = listOf(std::move(response->entries), response->truncated);
        result.mapEntries = response->mapEntries;
        result.mapBytes   = response->mapBytes;
        return result;
    }

    Result<ClientSession::PoolTagResult> ClientSession::poolTags(bool big, std::string_view tagFilter)
    {
        const std::string filterText {tagFilter};
        auto              response =
            runBound<PoolTagAnswer>(fd(),
                                    targetName(),
                                    [&](std::string& error)
                                    {
                                        return legacy::listPoolTags({}, {targetName(), big, filterText}, error);
                                    });
        if (!response)
        {
            return std::unexpected(response.error());
        }

        PoolTagResult result;
        result.tags       = listOf(std::move(response->tags), response->truncated);
        result.mapTags    = response->mapTags;
        result.mapEntries = response->mapEntries;
        return result;
    }

    Result<List<PfnDescriptor>> ClientSession::pfn(std::uint32_t pfnStart, std::uint32_t pfnCount)
    {
        auto response = runBound<PfnAnswer>(fd(),
                                            targetName(),
                                            [&](std::string& error)
                                            {
                                                return legacy::listPfn({}, {targetName(), pfnStart, pfnCount}, error);
                                            });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(response->entries));
    }

    Result<List<VmDescriptor>> ClientSession::vms()
    {
        auto response = runBound<std::vector<VmDescriptor>>(fd(),
                                                            targetName(),
                                                            [&](std::string& error)
                                                            {
                                                                return legacy::listVms({}, targetName(), error);
                                                            });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(*response));
    }

    Result<std::uint64_t> ClientSession::setConfig(ConfigOption option, std::uint32_t pid, std::uint64_t value)
    {
        return runBound<std::uint64_t>(fd(),
                                       targetName(),
                                       [&](std::string& error)
                                       {
                                           return legacy::setConfig({}, {targetName(), option, pid, value}, error);
                                       });
    }

    Result<SearchResult> ClientSession::searchMemory(SearchTerm    term,
                                                     std::uint32_t pid,
                                                     std::uint32_t alignBytes,
                                                     std::uint64_t minAddress,
                                                     std::uint64_t maxAddress,
                                                     std::uint32_t maxResults)
    {
        return runBound<SearchResult>(
            fd(),
            targetName(),
            [&](std::string& error)
            {
                return legacy::searchMemory(
                    {}, {targetName(), pid, term, alignBytes, minAddress, maxAddress, maxResults}, error);
            });
    }

    Result<VirtualRegion> ClientSession::queryVirtual(std::uint32_t pid, std::uint64_t address)
    {
        return runBound<VirtualRegion>(fd(),
                                       targetName(),
                                       [&](std::string& error)
                                       {
                                           return legacy::queryVirtual({}, {targetName(), pid, address}, error);
                                       });
    }

    Result<List<PageReadItem>> ClientSession::readPages(std::uint32_t pid, const std::vector<PageRange>& ranges)
    {
        auto response =
            runBound<std::vector<PageReadItem>>(fd(),
                                                targetName(),
                                                [&](std::string& error)
                                                {
                                                    return legacy::readPages({}, {targetName(), pid, ranges}, error);
                                                });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(*response));
    }

    Result<std::uint32_t> ClientSession::prefetchPages(std::uint32_t pid, const std::vector<PageRange>& ranges)
    {
        return runBound<std::uint32_t>(fd(),
                                       targetName(),
                                       [&](std::string& error)
                                       {
                                           return legacy::prefetchPages({}, {targetName(), pid, ranges}, error);
                                       });
    }

    Result<std::vector<std::byte>>
    ClientSession::readMemory(std::uint32_t pid, std::uint64_t address, std::uint32_t size)
    {
        if (size == 0)
        {
            return std::unexpected(invalidArgument("read size must be at least one byte"));
        }
        return runBound<std::vector<std::byte>>(fd(),
                                                targetName(),
                                                [&](std::string& error)
                                                {
                                                    return legacy::readMemory(
                                                        {}, {targetName(), pid, address, size}, error);
                                                });
    }

    Result<void> ClientSession::readMemoryInto(std::uint32_t pid, std::uint64_t address, std::span<std::byte> out)
    {
        if (out.empty())
        {
            return std::unexpected(invalidArgument("read size must be at least one byte"));
        }
        const auto data = ClientSession::readMemory(pid, address, static_cast<std::uint32_t>(out.size()));
        if (!data)
        {
            return std::unexpected(data.error());
        }
        std::copy(data->begin(), data->end(), out.begin());
        return {};
    }

    Result<std::uint64_t>
    ClientSession::writeMemory(std::uint32_t pid, std::uint64_t address, std::span<const std::byte> data)
    {
        if (data.empty())
        {
            return std::unexpected(invalidArgument("write payload is empty"));
        }
        return runBound<std::uint64_t>(
            fd(),
            targetName(),
            [&](std::string& error)
            {
                return legacy::writeMemory(
                    {}, {targetName(), pid, address, std::vector<std::byte>(data.begin(), data.end())}, error);
            });
    }

    Result<std::vector<ScatterReadResult>> ClientSession::scatterRead(std::uint32_t                    pid,
                                                                      const std::vector<ScatterRange>& ranges)
    {
        if (ranges.empty())
        {
            return std::unexpected(invalidArgument("scatter read needs at least one range"));
        }
        return runBound<std::vector<ScatterReadResult>>(fd(),
                                                        targetName(),
                                                        [&](std::string& error)
                                                        {
                                                            return legacy::scatterRead(
                                                                {}, {targetName(), pid, ranges}, error);
                                                        });
    }

    Result<Scatter> ClientSession::scatter(std::uint32_t pid)
    {
        if (!_connection)
        {
            return std::unexpected(Error {ErrorCode::ConnectionFailed, "session is closed"});
        }

        detail::resetLastErrorCode();
        std::string error;
        const auto  response =
            detail::exchange(fd(), MessageType::ScatterCreateRequest, encodeScatterCreateRequest({pid}), error);
        if (!response)
        {
            return std::unexpected(makeError(error));
        }

        const auto payload = detail::expectResponse(*response, MessageType::ScatterCreateResponse, error);
        if (!payload)
        {
            return std::unexpected(makeError(error));
        }

        const auto decoded = decodeScatterCreateResponse(*payload);
        if (!decoded)
        {
            return std::unexpected(Error {ErrorCode::ProtocolError, "server returned a malformed scatter response"});
        }
        return Scatter {_connection, decoded->scatterId};
    }

    Result<std::vector<std::byte>>
    ClientSession::readPhysicalMemory(std::uint32_t vmIndex, std::uint64_t gpa, std::uint32_t size)
    {
        if (size == 0)
        {
            return std::unexpected(invalidArgument("read size must be at least one byte"));
        }
        return runBound<std::vector<std::byte>>(fd(),
                                                targetName(),
                                                [&](std::string& error)
                                                {
                                                    return legacy::readPhysicalMemory(
                                                        {}, {targetName(), vmIndex, gpa, size}, error);
                                                });
    }

    Result<std::uint64_t>
    ClientSession::writePhysicalMemory(std::uint32_t vmIndex, std::uint64_t gpa, std::span<const std::byte> data)
    {
        if (data.empty())
        {
            return std::unexpected(invalidArgument("write payload is empty"));
        }
        return runBound<std::uint64_t>(
            fd(),
            targetName(),
            [&](std::string& error)
            {
                return legacy::writePhysicalMemory(
                    {}, {targetName(), vmIndex, gpa, std::vector<std::byte>(data.begin(), data.end())}, error);
            });
    }

    Result<GpaTranslateResponse> ClientSession::translateGpa(std::uint32_t vmIndex, std::uint64_t gpa)
    {
        return runBound<GpaTranslateResponse>(fd(),
                                              targetName(),
                                              [&](std::string& error)
                                              {
                                                  return legacy::translateGpa({}, {targetName(), vmIndex, gpa}, error);
                                              });
    }

    Result<std::uint64_t> ClientSession::virt2Phys(std::uint32_t pid, std::uint64_t va)
    {
        return runBound<std::uint64_t>(fd(),
                                       targetName(),
                                       [&](std::string& error)
                                       {
                                           return legacy::virt2Phys({}, {targetName(), pid, va}, error);
                                       });
    }

    Result<std::vector<ScatterReadResult>> ClientSession::physicalScatter(std::uint32_t                   vmIndex,
                                                                          const std::vector<ScatterItem>& entries)
    {
        if (entries.empty())
        {
            return std::unexpected(invalidArgument("physical scatter needs at least one entry"));
        }
        return runBound<std::vector<ScatterReadResult>>(fd(),
                                                        targetName(),
                                                        [&](std::string& error)
                                                        {
                                                            return legacy::physicalScatter(
                                                                {}, {targetName(), vmIndex, entries}, error);
                                                        });
    }

    Result<List<VfsEntry>> ClientSession::listVfs(std::string_view path, std::string_view nameFilter)
    {
        const std::string pathText {path};
        const std::string filterText {nameFilter};
        auto              response =
            runBound<std::vector<VfsEntry>>(fd(),
                                            targetName(),
                                            [&](std::string& error)
                                            {
                                                return legacy::listVfs({}, {targetName(), pathText, filterText}, error);
                                            });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(*response));
    }

    Result<std::vector<std::byte>>
    ClientSession::readVfs(std::string_view path, std::uint64_t offset, std::uint32_t size)
    {
        const std::string pathText {path};
        return runBound<std::vector<std::byte>>(fd(),
                                                targetName(),
                                                [&](std::string& error)
                                                {
                                                    return legacy::readVfs(
                                                        {}, {targetName(), pathText, offset, size}, error);
                                                });
    }

    Result<std::uint64_t>
    ClientSession::writeVfs(std::string_view path, std::uint64_t offset, std::span<const std::byte> data)
    {
        const std::string pathText {path};
        return runBound<std::uint64_t>(
            fd(),
            targetName(),
            [&](std::string& error)
            {
                return legacy::writeVfs(
                    {}, {targetName(), pathText, offset, std::vector<std::byte>(data.begin(), data.end())}, error);
            });
    }

    Result<DebugStatus> ClientSession::debugState()
    {
        return runBound<DebugStatus>(fd(),
                                     targetName(),
                                     [&](std::string& error)
                                     {
                                         return legacy::debugState({}, {targetName()}, error);
                                     });
    }

    Result<DebugStatus> ClientSession::debugControl(DebugControlAction action)
    {
        return runBound<DebugStatus>(fd(),
                                     targetName(),
                                     [&](std::string& error)
                                     {
                                         return legacy::debugControl({}, {targetName()}, action, error);
                                     });
    }

    Result<std::vector<DebugRegisterRecord>> ClientSession::debugRegisters(std::uint32_t vcpu)
    {
        return runBound<std::vector<DebugRegisterRecord>>(fd(),
                                                          targetName(),
                                                          [&](std::string& error)
                                                          {
                                                              return legacy::debugRegisters(
                                                                  {}, {targetName(), vcpu}, error);
                                                          });
    }

    Result<std::vector<DebugThreadRecord>> ClientSession::debugThreads()
    {
        return runBound<std::vector<DebugThreadRecord>>(fd(),
                                                        targetName(),
                                                        [&](std::string& error)
                                                        {
                                                            return legacy::debugThreads({}, {targetName()}, error);
                                                        });
    }

    Result<DisassembleResponse>
    ClientSession::disassemble(std::uint32_t pid, std::uint64_t address, std::uint32_t count)
    {
        return runBound<DisassembleResponse>(fd(),
                                             targetName(),
                                             [&](std::string& error)
                                             {
                                                 return legacy::disassemble(
                                                     {}, {targetName(), pid, address, count}, error);
                                             });
    }

    Result<DebugBacktraceResponse> ClientSession::debugBacktrace(std::uint32_t    pid,
                                                                 std::uint32_t    vcpu,
                                                                 std::string_view threadId,
                                                                 std::uint32_t    maxFrames)
    {
        const std::string threadText {threadId};
        return runBound<DebugBacktraceResponse>(fd(),
                                                targetName(),
                                                [&](std::string& error)
                                                {
                                                    return legacy::debugBacktrace(
                                                        {}, {targetName(), pid, vcpu, threadText, maxFrames}, error);
                                                });
    }

    Result<DebugWatchEnd> ClientSession::debugWatch(const DebugWatchOptions&                               options,
                                                    const std::function<void(const DebugWatchData&)>&      onHit,
                                                    const std::function<void(const DebugWatchHeartbeat&)>& onHeartbeat)
    {
        return runBound<DebugWatchEnd>(fd(),
                                       targetName(),
                                       [&](std::string& error)
                                       {
                                           return legacy::debugWatch({},
                                                                     {targetName(),
                                                                      options.pid,
                                                                      options.address,
                                                                      options.width,
                                                                      options.access,
                                                                      options.maxHits,
                                                                      options.timeoutMs,
                                                                      options.module,
                                                                      options.symbol},
                                                                     onHit,
                                                                     onHeartbeat,
                                                                     error);
                                       });
    }

    Result<DebugBreakpointResponse> ClientSession::debugBreakpoint(const DebugBreakpointOptions& options)
    {
        return runBound<DebugBreakpointResponse>(fd(),
                                                 targetName(),
                                                 [&](std::string& error)
                                                 {
                                                     return legacy::debugBreakpoint({},
                                                                                    {targetName(),
                                                                                     options.pid,
                                                                                     options.address,
                                                                                     options.kind,
                                                                                     options.timeoutMs,
                                                                                     options.module,
                                                                                     options.symbol},
                                                                                    error);
                                                 });
    }

    Result<DebugStepResponse> ClientSession::debugStep(std::uint32_t vcpu, std::string_view threadId)
    {
        const std::string threadText {threadId};
        return runBound<DebugStepResponse>(fd(),
                                           targetName(),
                                           [&](std::string& error)
                                           {
                                               return legacy::debugStep({}, {targetName(), vcpu, threadText}, error);
                                           });
    }

    Result<DebugAttachResponse>
    ClientSession::debugAttach(const std::function<void(const DebugAttachResponse&)>& whileAttached)
    {
        return runBound<DebugAttachResponse>(fd(),
                                             targetName(),
                                             [&](std::string& error)
                                             {
                                                 return legacy::debugAttach({}, {targetName()}, whileAttached, error);
                                             });
    }

    Result<DebugDetachResponse> ClientSession::debugDetach()
    {
        return runBound<DebugDetachResponse>(fd(),
                                             targetName(),
                                             [&](std::string& error)
                                             {
                                                 return legacy::debugDetach({}, {targetName()}, error);
                                             });
    }

    Result<DebugBreakpointArmResponse> ClientSession::debugArmBreakpoint(const DebugBreakpointOptions& options)
    {
        return runBound<DebugBreakpointArmResponse>(
            fd(),
            targetName(),
            [&](std::string& error)
            {
                return legacy::debugArmBreakpoint(
                    {}, {targetName(), options.pid, options.address, options.kind}, error);
            });
    }

    Result<DebugResourceRemoveResponse> ClientSession::debugRemoveBreakpoint(std::uint32_t id)
    {
        return runBound<DebugResourceRemoveResponse>(fd(),
                                                     targetName(),
                                                     [&](std::string& error)
                                                     {
                                                         return legacy::debugRemoveBreakpoint(
                                                             {}, {targetName(), id}, error);
                                                     });
    }

    Result<List<DebugBreakpointArmResponse>> ClientSession::debugListBreakpoints()
    {
        auto response = runBound<std::vector<DebugBreakpointArmResponse>>(fd(),
                                                                          targetName(),
                                                                          [&](std::string& error)
                                                                          {
                                                                              return legacy::debugListBreakpoints(
                                                                                  {}, {targetName()}, error);
                                                                          });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(*response));
    }

    Result<DebugWatchArmResponse> ClientSession::debugArmWatch(const DebugWatchOptions& options)
    {
        return runBound<DebugWatchArmResponse>(
            fd(),
            targetName(),
            [&](std::string& error)
            {
                return legacy::debugArmWatch(
                    {}, {targetName(), options.pid, options.address, options.width, options.access}, error);
            });
    }

    Result<DebugResourceRemoveResponse> ClientSession::debugRemoveWatch(std::uint32_t id)
    {
        return runBound<DebugResourceRemoveResponse>(fd(),
                                                     targetName(),
                                                     [&](std::string& error)
                                                     {
                                                         return legacy::debugRemoveWatch({}, {targetName(), id}, error);
                                                     });
    }

    Result<List<DebugWatchArmResponse>> ClientSession::debugListWatches()
    {
        auto response =
            runBound<std::vector<DebugWatchArmResponse>>(fd(),
                                                         targetName(),
                                                         [&](std::string& error)
                                                         {
                                                             return legacy::debugListWatches({}, {targetName()}, error);
                                                         });
        if (!response)
        {
            return std::unexpected(response.error());
        }
        return listOf(std::move(*response));
    }

    Result<DebugStatus> ClientSession::debugContinueArmed()
    {
        return runBound<DebugStatus>(fd(),
                                     targetName(),
                                     [&](std::string& error)
                                     {
                                         return legacy::debugContinueArmed({}, {targetName()}, error);
                                     });
    }

    Result<DebugWaitEnd> ClientSession::debugWait(std::uint32_t                                   timeoutMs,
                                                  std::uint32_t                                   maxHits,
                                                  const std::function<void(const DebugWaitHit&)>& onHit)
    {
        return runBound<DebugWaitEnd>(fd(),
                                      targetName(),
                                      [&](std::string& error)
                                      {
                                          return legacy::debugWait(
                                              {}, {targetName(), timeoutMs, maxHits}, onHit, error);
                                      });
    }

    Scatter::Scatter(std::shared_ptr<detail::Connection> connection, std::uint32_t scatterId)
        : _connection {std::move(connection)}, _scatterId {scatterId}
    {
    }

    Scatter::Scatter(Scatter&& other) noexcept
        : _connection {std::move(other._connection)}, _scatterId {other._scatterId}, _closed {other._closed}
    {
        other._scatterId = 0;
        other._closed    = true;
    }

    Scatter& Scatter::operator=(Scatter&& other) noexcept
    {
        if (this != &other)
        {
            (void)close();
            _connection      = std::move(other._connection);
            _scatterId       = other._scatterId;
            _closed          = other._closed;
            other._scatterId = 0;
            other._closed    = true;
        }
        return *this;
    }

    Scatter::~Scatter()
    {
        if (!_closed)
        {
            (void)close();
        }
    }

    std::uint32_t Scatter::id() const noexcept
    {
        return _scatterId;
    }

    Result<std::uint32_t> Scatter::prepare(const std::vector<ScatterItem>& entries)
    {
        if (!_connection)
        {
            return std::unexpected(Error {ErrorCode::ConnectionFailed, "scatter session is closed"});
        }
        ScatterSession session {_connection->fd.fd(), _scatterId, false};
        return runDirect<std::uint32_t>(
            [&](std::string& error)
            {
                return session.prepare(entries, error);
            });
    }

    Result<std::vector<ScatterReadResult>> Scatter::execute()
    {
        if (!_connection)
        {
            return std::unexpected(Error {ErrorCode::ConnectionFailed, "scatter session is closed"});
        }
        ScatterSession session {_connection->fd.fd(), _scatterId, false};
        return runDirect<std::vector<ScatterReadResult>>(
            [&](std::string& error)
            {
                return session.execute(error);
            });
    }

    Result<std::vector<ScatterReadResult>> Scatter::executeRead()
    {
        if (!_connection)
        {
            return std::unexpected(Error {ErrorCode::ConnectionFailed, "scatter session is closed"});
        }
        ScatterSession session {_connection->fd.fd(), _scatterId, false};
        return runDirect<std::vector<ScatterReadResult>>(
            [&](std::string& error)
            {
                return session.executeRead(error);
            });
    }

    Result<void> Scatter::clear()
    {
        if (!_connection)
        {
            return std::unexpected(Error {ErrorCode::ConnectionFailed, "scatter session is closed"});
        }
        ScatterSession session {_connection->fd.fd(), _scatterId, false};
        return runDirectVoid(
            [&](std::string& error)
            {
                return session.clear(error);
            });
    }

    Result<void> Scatter::close()
    {
        if (_closed)
        {
            return {};
        }
        if (!_connection)
        {
            _closed = true;
            return {};
        }
        ScatterSession session {_connection->fd.fd(), _scatterId, false};
        const auto     result = runDirectVoid(
            [&](std::string& error)
            {
                return session.close(error);
            });
        _closed = true;
        return result;
    }
} // namespace mpfshost
