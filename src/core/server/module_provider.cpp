#include "core/server/module_provider.hpp"

#include "core/util/text.hpp"

namespace mpfshost
{
    bool matchesModuleFilter(const ModuleListRequest& request, const ModuleDescriptor& module)
    {
        return containsCaseInsensitive(module.name, request.nameFilter);
    }
} // namespace mpfshost
