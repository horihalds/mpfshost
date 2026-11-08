#include "host/vmm_handle.hpp"

#include "core/debug/debug_controller.hpp"

namespace mpfshost
{
    std::optional<DebugStatus> VmmHandle::debugState(std::string& error)
    {
        return _debug.state(error);
    }

    std::optional<DebugStatus> VmmHandle::debugControl(DebugControlAction action, std::string& error)
    {
        return _debug.control(action, error);
    }

    std::optional<std::vector<DebugRegisterRecord>>
    VmmHandle::debugRegisters(const DebugRegistersRequest& request, bool& invalidState, std::string& error)
    {
        return _debug.registers(request, invalidState, error);
    }
} // namespace mpfshost
