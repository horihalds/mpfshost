#include "core/server/user_provider.hpp"

#include "core/util/text.hpp"

namespace mpfshost
{
    bool matchesUserFilter(const UserListRequest& request, const UserDescriptor& user)
    {
        return containsCaseInsensitive(user.name, request.filter) || containsCaseInsensitive(user.sid, request.filter);
    }
} // namespace mpfshost
