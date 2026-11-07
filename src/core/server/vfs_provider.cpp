#include "core/server/vfs_provider.hpp"

#include "core/util/text.hpp"

namespace mpfshost
{
    bool matchesVfsFilter(const VfsListRequest& request, const VfsEntry& entry)
    {
        return containsCaseInsensitive(entry.name, request.nameFilter);
    }
} // namespace mpfshost
