#include "core/server/thread_provider.hpp"

namespace mpfshost
{
    bool matchesThreadFilter(const ThreadListRequest& request, const ThreadDescriptor& thread)
    {
        return request.tid == 0 || request.tid == thread.tid;
    }
} // namespace mpfshost
