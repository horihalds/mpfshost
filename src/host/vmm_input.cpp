#include "host/vmm_handle.hpp"

#include <string>

namespace mpfshost
{
    std::optional<InputEventResponse>
    VmmHandle::input(const InputEventRequest& request, std::string& error, bool& unavailable)
    {
        return _debug.input(request, error, unavailable);
    }
} // namespace mpfshost
