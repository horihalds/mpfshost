#include "core/server/session.hpp"

#include "core/ipc/message.hpp"
#include "core/log/log.hpp"
#include "core/server/backend.hpp"
#include "core/server/map_batching.hpp"
#include "core/server/memory_map_provider.hpp"
#include "core/server/module_lookup_provider.hpp"
#include "core/server/module_provider.hpp"
#include "core/server/module_table_provider.hpp"
#include "core/server/thread_provider.hpp"
#include "core/server/unloaded_module_provider.hpp"
#include "core/server/vad_ex_provider.hpp"

#include <vector>

namespace mpfshost
{
    namespace
    {
        std::uint64_t moduleEncodedSize(const ModuleDescriptor& module)
        {
            constexpr std::uint64_t kFixed = sizeof(std::uint64_t) * 2 + sizeof(std::uint32_t) * 4
                                           + sizeof(std::uint16_t) * 2 + sizeof(std::uint8_t)
                                           + sizeof(std::uint32_t) * 5;
            return kFixed + module.name.size() + module.fullName.size() + module.pdbGuid.size() + module.pdbName.size()
                 + module.version.size();
        }

        std::uint64_t regionEncodedSize(const MemoryMapRegion& region)
        {
            constexpr std::uint64_t kFixed = sizeof(std::uint64_t) * 2 + sizeof(std::uint32_t) * 2
                                           + sizeof(std::uint16_t) * 2 + sizeof(std::uint8_t) + sizeof(std::uint32_t);
            return kFixed + region.description.size();
        }

        std::uint64_t threadFrameEncodedSize(const ThreadFrame& frame)
        {
            constexpr std::uint64_t kFixed = sizeof(std::uint32_t) + sizeof(std::uint64_t) * 3 + sizeof(std::uint32_t)
                                           + sizeof(std::uint8_t) + sizeof(std::uint32_t) * 2;
            return kFixed + frame.module.size() + frame.function.size();
        }

        std::uint64_t threadEncodedSize(const ThreadDescriptor& thread)
        {
            constexpr std::uint64_t kFixed =
                sizeof(std::uint32_t) * 6 + sizeof(std::uint64_t) * 15 + sizeof(std::uint8_t) * 7;
            std::uint64_t size = kFixed;
            for (const auto& frame : thread.frames)
            {
                size += threadFrameEncodedSize(frame);
            }
            return size;
        }
    } // namespace

    void Session::handleModuleList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeModuleListRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed module list request");
            return;
        }

        auto* provider = dynamic_cast<ModuleProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list modules");
            return;
        }

        std::string error;
        auto        modules = provider->listModules(*request, error);
        if (!modules.has_value())
        {
            sendError(requestId, ErrorCode::Internal, error.empty() ? "module listing failed" : error);
            return;
        }

        for (std::size_t offset = 0; offset < modules->size();)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto     count = batchCount(*modules, offset, kMaxMapBatchBytes, moduleEncodedSize);
            ModuleListData data;
            data.modules.assign(modules->begin() + static_cast<std::ptrdiff_t>(offset),
                                modules->begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::ModuleListData, encodeModuleListData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const ModuleListEnd end {0, modules->size(), ""};
        sendPayload(requestId, MessageType::ModuleListEnd, encodeModuleListEnd(end));
    }

    void Session::handleModuleLayout(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeModuleLayoutRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed module layout request");
            return;
        }

        auto* provider = dynamic_cast<ModuleLookupProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot describe module layouts");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        if (auto response = provider->moduleLayout(*request, status, error); response.has_value())
        {
            sendPayload(requestId, MessageType::ModuleLayoutResponse, encodeModuleLayoutResponse(*response));
            return;
        }

        sendError(requestId,
                  status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                  error.empty() ? "module layout lookup failed" : error);
    }

    void Session::handleMemoryMap(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeMemoryMapRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed memory map request");
            return;
        }

        auto* provider = dynamic_cast<MemoryMapProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list memory maps");
            return;
        }

        std::string error;
        auto        regions = provider->listMemoryMap(*request, error);
        if (!regions.has_value())
        {
            sendError(requestId, ErrorCode::Internal, error.empty() ? "memory map listing failed" : error);
            return;
        }

        for (std::size_t offset = 0; offset < regions->size();)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto    count = batchCount(*regions, offset, kMaxMapBatchBytes, regionEncodedSize);
            MemoryMapData data;
            data.regions.assign(regions->begin() + static_cast<std::ptrdiff_t>(offset),
                                regions->begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::MemoryMapData, encodeMemoryMapData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const MemoryMapEnd end {0, regions->size(), ""};
        sendPayload(requestId, MessageType::MemoryMapEnd, encodeMemoryMapEnd(end));
    }

    void Session::handleVadExList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeVadExListRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed vad-ex list request");
            return;
        }
        if (request->pid == 0)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "vad-ex listing needs a process id");
            return;
        }
        if (request->pageCount == 0)
        {
            sendError(requestId, ErrorCode::InvalidRange, "vad-ex page count is zero");
            return;
        }
        if (request->pageCount > kMaxPageCount)
        {
            sendError(requestId, ErrorCode::ResultTooLarge, "vad-ex page count is above the page count cap");
            return;
        }

        auto* provider = dynamic_cast<VadExProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list vad-ex pages");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        auto         entries = provider->listVadEx(*request, status, error);
        if (!entries.has_value())
        {
            if (_deadline.expired())
            {
                abortRequest(requestId, ErrorCode::Timeout);
                return;
            }
            sendError(requestId,
                      status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                      error.empty() ? "vad-ex listing failed" : error);
            return;
        }

        for (std::size_t offset = 0; offset < entries->size();)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto    count = batchCount(*entries, offset, kMaxMapBatchBytes, vadExEncodedSize);
            VadExListData data;
            data.entries.assign(entries->begin() + static_cast<std::ptrdiff_t>(offset),
                                entries->begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::VadExListData, encodeVadExListData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const VadExListEnd end {0, entries->size(), ""};
        sendPayload(requestId, MessageType::VadExListEnd, encodeVadExListEnd(end));
    }

    void Session::handleThreadList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeThreadListRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed thread list request");
            return;
        }

        auto* provider = dynamic_cast<ThreadProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list threads");
            return;
        }

        std::string error;
        auto        threads = provider->listThreads(*request, error);
        if (!threads.has_value())
        {
            sendError(requestId, ErrorCode::Internal, error.empty() ? "thread listing failed" : error);
            return;
        }

        for (std::size_t offset = 0; offset < threads->size();)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto     count = batchCount(*threads, offset, kMaxMapBatchBytes, threadEncodedSize);
            ThreadListData data;
            data.threads.assign(threads->begin() + static_cast<std::ptrdiff_t>(offset),
                                threads->begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::ThreadListData, encodeThreadListData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const ThreadListEnd end {0, threads->size(), ""};
        sendPayload(requestId, MessageType::ThreadListEnd, encodeThreadListEnd(end));
    }

    void Session::handleExportList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeExportListRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed export list request");
            return;
        }
        if (request->pid == 0 || request->module.empty())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "export listing needs a process id and a module");
            return;
        }

        auto* provider = dynamic_cast<ModuleTableProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list exports");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        auto         result = provider->listExports(*request, status, error);
        if (!result.has_value())
        {
            sendError(requestId,
                      status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                      error.empty() ? "export listing failed" : error);
            return;
        }

        for (std::size_t offset = 0; offset < result->exports.size();)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto     count = batchCount(result->exports, offset, kMaxMapBatchBytes, exportEncodedSize);
            ExportListData data;
            data.exports.assign(result->exports.begin() + static_cast<std::ptrdiff_t>(offset),
                                result->exports.begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::ExportListData, encodeExportListData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const ExportListEnd end {0,
                                 static_cast<std::uint64_t>(result->exports.size()),
                                 static_cast<std::uint8_t>(result->truncated ? 1 : 0),
                                 ""};
        sendPayload(requestId, MessageType::ExportListEnd, encodeExportListEnd(end));
    }

    void Session::handleImportList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeImportListRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed import list request");
            return;
        }
        if (request->pid == 0 || request->module.empty())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "import listing needs a process id and a module");
            return;
        }

        auto* provider = dynamic_cast<ModuleTableProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list imports");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        auto         result = provider->listImports(*request, status, error);
        if (!result.has_value())
        {
            sendError(requestId,
                      status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                      error.empty() ? "import listing failed" : error);
            return;
        }

        for (std::size_t offset = 0; offset < result->imports.size();)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto     count = batchCount(result->imports, offset, kMaxMapBatchBytes, importEncodedSize);
            ImportListData data;
            data.imports.assign(result->imports.begin() + static_cast<std::ptrdiff_t>(offset),
                                result->imports.begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::ImportListData, encodeImportListData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const ImportListEnd end {0,
                                 static_cast<std::uint64_t>(result->imports.size()),
                                 static_cast<std::uint8_t>(result->truncated ? 1 : 0),
                                 ""};
        sendPayload(requestId, MessageType::ImportListEnd, encodeImportListEnd(end));
    }

    void Session::handleUnloadedModuleList(std::uint64_t requestId, std::span<const std::byte> payload)
    {
        if (!_bound.has_value())
        {
            sendError(requestId, ErrorCode::NotBound, "session is not bound to a target");
            return;
        }

        const auto request = decodeUnloadedModuleListRequest(payload);
        if (!request.has_value())
        {
            sendError(requestId, ErrorCode::MalformedPayload, "malformed unloaded module list request");
            return;
        }
        if (request->pid == 0)
        {
            sendError(requestId, ErrorCode::MalformedPayload, "unloaded module listing needs a process id");
            return;
        }

        auto* provider = dynamic_cast<UnloadedModuleProvider*>(_bound->handle.get());
        if (provider == nullptr)
        {
            sendError(requestId, ErrorCode::CapabilityUnavailable, "the bound target cannot list unloaded modules");
            return;
        }

        LookupStatus status = LookupStatus::Failed;
        std::string  error;
        auto         result = provider->listUnloadedModules(*request, status, error);
        if (!result.has_value())
        {
            sendError(requestId,
                      status == LookupStatus::NotFound ? ErrorCode::NotFound : ErrorCode::Internal,
                      error.empty() ? "unloaded module listing failed" : error);
            return;
        }

        for (std::size_t offset = 0; offset < result->modules.size();)
        {
            ErrorCode abortCode {};
            if (requestAborted(requestId, abortCode))
            {
                abortRequest(requestId, abortCode);
                return;
            }
            const auto count = batchCount(result->modules, offset, kMaxMapBatchBytes, unloadedModuleEncodedSize);
            UnloadedModuleListData data;
            data.modules.assign(result->modules.begin() + static_cast<std::ptrdiff_t>(offset),
                                result->modules.begin() + static_cast<std::ptrdiff_t>(offset + count));
            sendPayload(requestId, MessageType::UnloadedModuleListData, encodeUnloadedModuleListData(data));
            if (_writeFailed)
            {
                return;
            }
            offset += count;
        }

        const UnloadedModuleListEnd end {0,
                                         static_cast<std::uint64_t>(result->modules.size()),
                                         static_cast<std::uint8_t>(result->truncated ? 1 : 0),
                                         ""};
        sendPayload(requestId, MessageType::UnloadedModuleListEnd, encodeUnloadedModuleListEnd(end));
    }
} // namespace mpfshost
