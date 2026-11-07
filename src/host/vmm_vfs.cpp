#include "host/vmm_handle.hpp"

#include "core/ipc/frame.hpp"
#include "core/server/request_deadline.hpp"
#include "core/server/vfs_provider.hpp"
#include "host/vmm_text.hpp"

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mpfshost
{
    namespace
    {
        // Clients address the tree with either separator; MemProcFS itself only accepts backslashes and VFS names
        // never contain one, so the rewrite is unambiguous.
        std::string normaliseVfsPath(const std::string& path)
        {
            std::string normalised = path;
            for (char& character : normalised)
            {
                if (character == '/')
                {
                    character = '\\';
                }
            }
            return normalised;
        }

        struct VfsCollector
        {
            const VfsListRequest&  request;
            std::vector<VfsEntry>& entries;
            std::size_t            seen {};
        };

        struct VfsNameCollector
        {
            std::vector<std::string> names;
        };

        bool equalsIgnoreCase(std::string_view left, std::string_view right)
        {
            if (left.size() != right.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < left.size(); ++index)
            {
                if (std::tolower(static_cast<unsigned char>(left[index]))
                    != std::tolower(static_cast<unsigned char>(right[index])))
                {
                    return false;
                }
            }
            return true;
        }

        void copyTimes(PVMMDLL_VFS_FILELIST_EXINFO exInfo, VfsEntry& entry)
        {
            if (exInfo == nullptr)
            {
                return;
            }
            entry.creationTime   = exInfo->qwCreationTime;
            entry.lastAccessTime = exInfo->qwLastAccessTime;
            entry.lastWriteTime  = exInfo->qwLastWriteTime;
        }

        void collectEntry(
            VfsCollector& collector, LPCSTR name, ULONG64 size, bool isDirectory, PVMMDLL_VFS_FILELIST_EXINFO exInfo)
        {
            VfsEntry entry;
            entry.name        = copyMapText(name);
            entry.size        = size;
            entry.isDirectory = isDirectory;
            copyTimes(exInfo, entry);
            ++collector.seen;
            if (matchesVfsFilter(collector.request, entry))
            {
                collector.entries.push_back(std::move(entry));
            }
        }

        VOID addFile(HANDLE context, LPCSTR name, ULONG64 size, PVMMDLL_VFS_FILELIST_EXINFO exInfo)
        {
            auto* collector = static_cast<VfsCollector*>(context);
            if (collector != nullptr)
            {
                collectEntry(*collector, name, size, false, exInfo);
            }
        }

        VOID addDirectory(HANDLE context, LPCSTR name, PVMMDLL_VFS_FILELIST_EXINFO exInfo)
        {
            auto* collector = static_cast<VfsCollector*>(context);
            if (collector != nullptr)
            {
                collectEntry(*collector, name, 0, true, exInfo);
            }
        }

        void collectName(HANDLE context, LPCSTR name)
        {
            auto* collector = static_cast<VfsNameCollector*>(context);
            if (collector != nullptr && name != nullptr)
            {
                collector->names.push_back(copyMapText(name));
            }
        }

        VOID addNameFile(HANDLE context, LPCSTR name, ULONG64, PVMMDLL_VFS_FILELIST_EXINFO)
        {
            collectName(context, name);
        }

        VOID addNameDirectory(HANDLE context, LPCSTR name, PVMMDLL_VFS_FILELIST_EXINFO)
        {
            collectName(context, name);
        }

        // MemProcFS answers success with zero entries for a missing path, so a typo is indistinguishable from an
        // empty directory. Listing the parent and looking for the leaf separates the two.
        bool vfsPathExists(VMM_HANDLE handle, const std::string& path)
        {
            std::string trimmed = path;
            while (trimmed.size() > 1 && trimmed.back() == '\\')
            {
                trimmed.pop_back();
            }
            if (trimmed.size() <= 1)
            {
                return true;
            }

            const auto separator = trimmed.rfind('\\');
            if (separator == std::string::npos)
            {
                return true;
            }

            std::string parent = trimmed.substr(0, separator);
            const auto  leaf   = trimmed.substr(separator + 1);
            if (parent.empty())
            {
                parent = "\\";
            }
            // A top-level path has the root as its parent; keeping the old empty-success answer there avoids
            // treating a name the tree synthesises differently as missing.
            if (parent == "\\" || leaf.empty())
            {
                return true;
            }

            VfsNameCollector     names;
            VMMDLL_VFS_FILELIST2 list {};
            list.dwVersion       = VMMDLL_VFS_FILELIST_VERSION;
            list.pfnAddFile      = &addNameFile;
            list.pfnAddDirectory = &addNameDirectory;
            list.h               = &names;
            if (!VMMDLL_VfsListU(handle, parent.c_str(), &list))
            {
                return true;
            }
            for (const auto& name : names.names)
            {
                if (equalsIgnoreCase(name, leaf))
                {
                    return true;
                }
            }
            return false;
        }
    } // namespace

    std::optional<std::vector<VfsEntry>>
    VmmHandle::listVfs(const VfsListRequest& request, LookupStatus& status, std::string& error)
    {
        if (_handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return std::nullopt;
        }

        if (requestExpired())
        {
            error = "request deadline exceeded";
            return std::nullopt;
        }

        std::vector<VfsEntry> entries;
        VfsCollector          collector {request, entries};

        VMMDLL_VFS_FILELIST2 list {};
        list.dwVersion       = VMMDLL_VFS_FILELIST_VERSION;
        list.pfnAddFile      = &addFile;
        list.pfnAddDirectory = &addDirectory;
        list.h               = &collector;

        const std::string path = normaliseVfsPath(request.path);
        if (!VMMDLL_VfsListU(_handle, path.c_str(), &list))
        {
            error = "VMMDLL_VfsListU failed for path '" + path + "'";
            return std::nullopt;
        }
        if (requestExpired())
        {
            error = "request deadline exceeded";
            return std::nullopt;
        }
        if (collector.seen == 0 && !vfsPathExists(_handle, path))
        {
            status = LookupStatus::NotFound;
            error  = "vfs listing failed: path '" + path + "' is not present";
            return std::nullopt;
        }
        status = LookupStatus::Ok;
        return entries;
    }

    bool VmmHandle::readVfs(const VfsReadRequest& request, std::vector<std::byte>& out, std::string& error)
    {
        if (_handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return false;
        }

        if (requestExpired())
        {
            error = "request deadline exceeded";
            return false;
        }

        std::vector<std::byte> buffer(request.size);
        DWORD                  bytesRead = 0;

        const std::string path   = normaliseVfsPath(request.path);
        const NTSTATUS    status = VMMDLL_VfsReadU(_handle,
                                                   path.c_str(),
                                                   reinterpret_cast<PBYTE>(buffer.data()),
                                                   static_cast<DWORD>(request.size),
                                                   &bytesRead,
                                                   request.offset);
        if (status != VMMDLL_STATUS_SUCCESS && status != VMMDLL_STATUS_END_OF_FILE)
        {
            error = "VMMDLL_VfsReadU failed for path '" + path + "' with status " + formatStatus(status);
            return false;
        }

        if (requestExpired())
        {
            error = "request deadline exceeded";
            return false;
        }
        if (bytesRead > buffer.size())
        {
            error = "VMMDLL_VfsReadU reported more bytes than requested";
            return false;
        }
        buffer.resize(bytesRead);
        out = std::move(buffer);
        return true;
    }

    bool VmmHandle::writeVfs(const VfsWriteRequest& request, std::uint32_t& bytesWritten, std::string& error)
    {
        if (_handle == nullptr)
        {
            error = "no MemProcFS handle is available";
            return false;
        }

        if (requestExpired())
        {
            error = "request deadline exceeded";
            return false;
        }

        std::vector<std::byte> buffer {request.data.begin(), request.data.end()};
        DWORD                  written = 0;

        const std::string path   = normaliseVfsPath(request.path);
        const NTSTATUS    status = VMMDLL_VfsWriteU(_handle,
                                                    path.c_str(),
                                                    reinterpret_cast<PBYTE>(buffer.data()),
                                                    static_cast<DWORD>(buffer.size()),
                                                    &written,
                                                    request.offset);
        if (status != VMMDLL_STATUS_SUCCESS)
        {
            error = "VMMDLL_VfsWriteU failed for path '" + path + "' with status " + formatStatus(status);
            return false;
        }

        if (requestExpired())
        {
            error = "request deadline exceeded";
            return false;
        }
        if (written > buffer.size())
        {
            error = "VMMDLL_VfsWriteU reported more bytes than requested";
            return false;
        }
        bytesWritten = written;
        return true;
    }
} // namespace mpfshost
