#include "core/server/unloaded_module_provider.hpp"

#include "core/util/text.hpp"

namespace mpfshost
{
    bool matchesUnloadedModuleFilter(const UnloadedModuleListRequest& request, const UnloadedModuleDescriptor& module)
    {
        return containsCaseInsensitive(module.text, request.filter);
    }
} // namespace mpfshost
