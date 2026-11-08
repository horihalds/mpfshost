#include "host/vmm_handle_factory.hpp"

#include "core/log/log.hpp"
#include "host/device_string.hpp"
#include "host/vmm_handle.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <thread>

#include "vmmdll.h"

namespace mpfshost
{
    namespace
    {
        std::shared_ptr<VmHandle> createHandle(const VmTarget& target, std::string& error)
        {
            log::info("host", "creating VMM handle for target ", target.name);
            const auto device = qemuDeviceString(target);
            log::debug("host", "target ", target.name, " device=", device);
            LPCSTR params[] {"", "-waitinitialize", "-disable-python", "-device", device.c_str()};

            VMM_HANDLE handle = VMMDLL_Initialize(std::size(params), params);
            if (handle == nullptr)
            {
                error = "VMMDLL_Initialize failed for target '" + target.name + "'";
                log::error("host", error);
                return nullptr;
            }

            if (!VMMDLL_InitializePlugins(handle))
            {
                VMMDLL_Close(handle);
                error = "VMMDLL_InitializePlugins failed for target '" + target.name + "'";
                log::error("host", error);
                return nullptr;
            }

            // MemProcFS needs a moment after plugin initialization before the handle is usable.
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            return std::make_shared<VmmHandle>(handle, target.name, target.qmpSocket);
        }
    } // namespace

    VmmHandleFactory::~VmmHandleFactory()
    {
        // Shutdown must not leave a handle behind. A live one here means something outlived the cache, so the
        // count is reported before CloseAll reclaims it; the declaration order in runServer keeps it zero.
        std::size_t alive = 0;
        {
            std::lock_guard lock {_mutex};
            alive = static_cast<std::size_t>(std::ranges::count_if(_handles,
                                                                   [](const std::weak_ptr<VmHandle>& handle)
                                                                   {
                                                                       return !handle.expired();
                                                                   }));
            _handles.clear();
        }

        if (alive != 0)
        {
            log::warn("host", alive, " VMM handle(s) still alive at shutdown");
        }
        log::info("host", "closing every remaining VMM handle");
        VMMDLL_CloseAll();
    }

    std::shared_ptr<VmHandle> VmmHandleFactory::acquire(const VmTarget& target, std::string& error)
    {
        auto handle = createHandle(target, error);
        if (!handle)
        {
            return nullptr;
        }

        std::lock_guard lock {_mutex};
        std::erase_if(_handles,
                      [](const std::weak_ptr<VmHandle>& tracked)
                      {
                          return tracked.expired();
                      });
        _handles.push_back(handle);
        return handle;
    }
} // namespace mpfshost
