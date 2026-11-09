#include "core/client/client_legacy.hpp"

#include "core/client/client_transport.hpp"

namespace mpfshost
{
    using detail::connectAndBind;
    using detail::exchange;
    using detail::exchangeStreamed;
    using detail::expectResponse;

    std::optional<std::vector<ModuleDescriptor>>
    listModules(const std::filesystem::path& socketPath, const ModuleQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::ModuleListRequest,
                                               encodeModuleListRequest({query.pid, query.flags, query.nameFilter}),
                                               MessageType::ModuleListEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodeModuleListEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed module list end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "module listing failed" : end->error;
            return std::nullopt;
        }

        std::vector<ModuleDescriptor> modules;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodeModuleListData(frame);
            if (!decoded)
            {
                error = "server returned malformed module list data";
                return std::nullopt;
            }
            modules.insert(modules.end(), decoded->modules.begin(), decoded->modules.end());
        }
        if (modules.size() != end->moduleCount)
        {
            error = "server reported " + std::to_string(end->moduleCount) + " modules but delivered "
                  + std::to_string(modules.size());
            return std::nullopt;
        }
        return modules;
    }

    std::optional<ModuleLayoutResponse>
    moduleLayout(const std::filesystem::path& socketPath, const ModuleLayoutQuery& query, std::string& error)
    {
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto requestPayload = encodeModuleLayoutRequest({query.pid, query.module});
        const auto request        = exchange(connection->fd(), MessageType::ModuleLayoutRequest, requestPayload, error);
        if (!request)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*request, MessageType::ModuleLayoutResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeModuleLayoutResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed module layout response";
        }
        return decoded;
    }

    std::optional<ModuleByAddressResponse>
    moduleByAddress(const std::filesystem::path& socketPath, const ModuleByAddressQuery& query, std::string& error)
    {
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto requestPayload = encodeModuleByAddressRequest({query.pid, query.address});
        const auto request = exchange(connection->fd(), MessageType::ModuleByAddressRequest, requestPayload, error);
        if (!request)
        {
            return std::nullopt;
        }

        const auto payload = expectResponse(*request, MessageType::ModuleByAddressResponse, error);
        if (!payload)
        {
            return std::nullopt;
        }

        const auto decoded = decodeModuleByAddressResponse(*payload);
        if (!decoded)
        {
            error = "server returned a malformed module-by-address response";
        }
        return decoded;
    }

    std::optional<AddressResolution>
    resolveAddress(const std::filesystem::path& socketPath, const AddressResolutionQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto moduleRequest = exchange(connection->fd(),
                                            MessageType::ModuleByAddressRequest,
                                            encodeModuleByAddressRequest({query.pid, query.address}),
                                            error);
        if (!moduleRequest)
        {
            return std::nullopt;
        }

        ErrorCode  moduleError = ErrorCode::Internal;
        const auto modulePayload =
            expectResponse(*moduleRequest, MessageType::ModuleByAddressResponse, error, &moduleError);
        if (!modulePayload)
        {
            if (moduleError == ErrorCode::NotFound)
            {
                error.clear();
                return AddressResolution {};
            }
            return std::nullopt;
        }

        const auto module = decodeModuleByAddressResponse(*modulePayload);
        if (!module)
        {
            error = "server returned a malformed module-by-address response";
            return std::nullopt;
        }

        AddressResolution resolution;
        resolution.moduleFound     = true;
        resolution.module          = module->name;
        resolution.moduleFullName  = module->fullName;
        resolution.moduleBase      = module->base;
        resolution.moduleEntry     = module->entry;
        resolution.moduleImageSize = module->imageSize;
        resolution.moduleOffset    = module->offset;
        resolution.moduleKind      = module->kind;
        resolution.moduleWow64     = module->wow64;

        const auto exports = exchangeStreamed(connection->fd(),
                                              MessageType::ExportListRequest,
                                              encodeExportListRequest({query.pid, module->name}),
                                              MessageType::ExportListEnd,
                                              error);
        if (!exports)
        {
            return std::nullopt;
        }

        const auto exportEnd = decodeExportListEnd(exports->endPayload);
        if (!exportEnd)
        {
            error = "server returned a malformed export list end";
            return std::nullopt;
        }

        if (exportEnd->status == 0)
        {
            std::optional<ExportDescriptor> best;
            for (const auto& frame : exports->data)
            {
                const auto decoded = decodeExportListData(frame);
                if (!decoded)
                {
                    error = "server returned malformed export list data";
                    return std::nullopt;
                }
                for (const auto& entry : decoded->exports)
                {
                    if (entry.vaFunction == 0 || entry.vaFunction > query.address)
                    {
                        continue;
                    }
                    if (!best.has_value() || entry.vaFunction > best->vaFunction
                        || (entry.vaFunction == best->vaFunction && best->function.empty()))
                    {
                        best = entry;
                    }
                }
            }
            if (best.has_value())
            {
                resolution.exportFound   = true;
                resolution.exportAddress = best->vaFunction;
                resolution.exportOffset  = query.address - best->vaFunction;
                resolution.exportName    = best->function;
            }
        }

        // A missing or unloaded PDB is normal; only a well-formed symbol response is recorded.
        std::string pdbError;
        const auto  pdbRequest = exchange(connection->fd(),
                                          MessageType::PdbSymbolNameRequest,
                                          encodePdbSymbolNameRequest({module->name, module->offset}),
                                          pdbError);
        if (pdbRequest && pdbRequest->type != MessageType::ErrorResponse)
        {
            if (const auto pdbPayload = expectResponse(*pdbRequest, MessageType::PdbSymbolNameResponse, pdbError))
            {
                if (const auto pdb = decodePdbSymbolNameResponse(*pdbPayload))
                {
                    resolution.pdbFound        = true;
                    resolution.pdbSymbol       = pdb->symbol;
                    resolution.pdbDisplacement = pdb->displacement;
                }
            }
        }

        return resolution;
    }

    std::optional<std::vector<MemoryMapRegion>>
    listMemoryMap(const std::filesystem::path& socketPath, const MemoryMapQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::MemoryMapRequest,
                                               encodeMemoryMapRequest({query.pid, query.identifyModules}),
                                               MessageType::MemoryMapEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodeMemoryMapEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed memory map end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "memory map listing failed" : end->error;
            return std::nullopt;
        }

        std::vector<MemoryMapRegion> regions;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodeMemoryMapData(frame);
            if (!decoded)
            {
                error = "server returned malformed memory map data";
                return std::nullopt;
            }
            regions.insert(regions.end(), decoded->regions.begin(), decoded->regions.end());
        }
        if (regions.size() != end->regionCount)
        {
            error = "server reported " + std::to_string(end->regionCount) + " regions but delivered "
                  + std::to_string(regions.size());
            return std::nullopt;
        }
        return regions;
    }

    std::optional<std::vector<VadExDescriptor>>
    listVadEx(const std::filesystem::path& socketPath, const VadExQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::VadExListRequest,
                                               encodeVadExListRequest({query.pid, query.pageOffset, query.pageCount}),
                                               MessageType::VadExListEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodeVadExListEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed vad-ex list end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "vad-ex listing failed" : end->error;
            return std::nullopt;
        }

        std::vector<VadExDescriptor> entries;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodeVadExListData(frame);
            if (!decoded)
            {
                error = "server returned malformed vad-ex list data";
                return std::nullopt;
            }
            entries.insert(entries.end(), decoded->entries.begin(), decoded->entries.end());
        }
        if (entries.size() != end->entryCount)
        {
            error = "server reported " + std::to_string(end->entryCount) + " vad-ex pages but delivered "
                  + std::to_string(entries.size());
            return std::nullopt;
        }
        return entries;
    }

    std::optional<std::vector<ThreadDescriptor>>
    listThreads(const std::filesystem::path& socketPath, const ThreadQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::ThreadListRequest,
                                               encodeThreadListRequest({query.pid, query.tid, query.flags}),
                                               MessageType::ThreadListEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodeThreadListEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed thread list end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "thread listing failed" : end->error;
            return std::nullopt;
        }

        std::vector<ThreadDescriptor> threads;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodeThreadListData(frame);
            if (!decoded)
            {
                error = "server returned malformed thread list data";
                return std::nullopt;
            }
            threads.insert(threads.end(), decoded->threads.begin(), decoded->threads.end());
        }
        if (threads.size() != end->threadCount)
        {
            error = "server reported " + std::to_string(end->threadCount) + " threads but delivered "
                  + std::to_string(threads.size());
            return std::nullopt;
        }
        return threads;
    }

    std::optional<std::vector<ExportDescriptor>>
    listExports(const std::filesystem::path& socketPath, const ModuleTableQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::ExportListRequest,
                                               encodeExportListRequest({query.pid, query.module}),
                                               MessageType::ExportListEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodeExportListEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed export list end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "export listing failed" : end->error;
            return std::nullopt;
        }

        std::vector<ExportDescriptor> exports;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodeExportListData(frame);
            if (!decoded)
            {
                error = "server returned malformed export list data";
                return std::nullopt;
            }
            exports.insert(exports.end(), decoded->exports.begin(), decoded->exports.end());
        }
        if (exports.size() != end->exportCount)
        {
            error = "server reported " + std::to_string(end->exportCount) + " exports but delivered "
                  + std::to_string(exports.size());
            return std::nullopt;
        }
        return exports;
    }

    std::optional<std::vector<ImportDescriptor>>
    listImports(const std::filesystem::path& socketPath, const ModuleTableQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::ImportListRequest,
                                               encodeImportListRequest({query.pid, query.module}),
                                               MessageType::ImportListEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodeImportListEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed import list end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "import listing failed" : end->error;
            return std::nullopt;
        }

        std::vector<ImportDescriptor> imports;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodeImportListData(frame);
            if (!decoded)
            {
                error = "server returned malformed import list data";
                return std::nullopt;
            }
            imports.insert(imports.end(), decoded->imports.begin(), decoded->imports.end());
        }
        if (imports.size() != end->importCount)
        {
            error = "server reported " + std::to_string(end->importCount) + " imports but delivered "
                  + std::to_string(imports.size());
            return std::nullopt;
        }
        return imports;
    }

    std::optional<std::vector<UnloadedModuleDescriptor>>
    listUnloadedModules(const std::filesystem::path& socketPath, const UnloadedModuleQuery& query, std::string& error)
    {
        error.clear();
        const auto connection = connectAndBind(socketPath, query.targetName, error);
        if (!connection)
        {
            return std::nullopt;
        }

        const auto streamed = exchangeStreamed(connection->fd(),
                                               MessageType::UnloadedModuleListRequest,
                                               encodeUnloadedModuleListRequest({query.pid, query.filter}),
                                               MessageType::UnloadedModuleListEnd,
                                               error);
        if (!streamed)
        {
            return std::nullopt;
        }

        const auto end = decodeUnloadedModuleListEnd(streamed->endPayload);
        if (!end)
        {
            error = "server returned a malformed unloaded module list end";
            return std::nullopt;
        }
        if (end->status != 0)
        {
            error = end->error.empty() ? "unloaded module listing failed" : end->error;
            return std::nullopt;
        }

        std::vector<UnloadedModuleDescriptor> modules;
        for (const auto& frame : streamed->data)
        {
            const auto decoded = decodeUnloadedModuleListData(frame);
            if (!decoded)
            {
                error = "server returned malformed unloaded module list data";
                return std::nullopt;
            }
            modules.insert(modules.end(), decoded->modules.begin(), decoded->modules.end());
        }
        if (modules.size() != end->moduleCount)
        {
            error = "server reported " + std::to_string(end->moduleCount) + " unloaded modules but delivered "
                  + std::to_string(modules.size());
            return std::nullopt;
        }
        return modules;
    }

} // namespace mpfshost
