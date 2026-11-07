#include "core/server/handle_provider.hpp"

#include "core/util/text.hpp"

namespace mpfshost
{
    bool matchesHandleFilter(const HandleListRequest& request, const HandleDescriptor& handle)
    {
        // Both the object name and the object type are compared: a client looking for "file" wants every File
        // handle, and one looking for "svc" wants the Service handles even when they carry no object name.
        return containsCaseInsensitive(handle.text, request.filter)
            || containsCaseInsensitive(handle.type, request.filter);
    }
} // namespace mpfshost
