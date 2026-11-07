#include "core/server/service_provider.hpp"

#include "core/util/text.hpp"

namespace mpfshost
{
    bool matchesServiceFilter(const ServiceListRequest& request, const ServiceDescriptor& service)
    {
        return containsCaseInsensitive(service.name, request.filter)
            || containsCaseInsensitive(service.displayName, request.filter)
            || containsCaseInsensitive(service.imagePath, request.filter)
            || containsCaseInsensitive(service.path, request.filter)
            || containsCaseInsensitive(service.userType, request.filter)
            || containsCaseInsensitive(service.account, request.filter);
    }
} // namespace mpfshost
