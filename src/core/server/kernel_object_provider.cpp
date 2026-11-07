#include "core/server/kernel_object_provider.hpp"

#include "core/util/text.hpp"

namespace mpfshost
{
    bool matchesKernelObjectFilter(const KObjectListRequest& request, const KernelObjectDescriptor& object)
    {
        return containsCaseInsensitive(object.name, request.filter)
            || containsCaseInsensitive(object.type, request.filter);
    }
} // namespace mpfshost
