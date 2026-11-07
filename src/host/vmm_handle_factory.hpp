#ifndef MPFSHOST_HOST_VMM_HANDLE_FACTORY_HPP
#define MPFSHOST_HOST_VMM_HANDLE_FACTORY_HPP
#pragma once

#include "core/server/backend.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mpfshost
{
    // Creates a fresh MemProcFS handle per acquisition; the handle cache above decides how long one is kept.
    class VmmHandleFactory final : public VmBackend
    {
    public:
        VmmHandleFactory() = default;
        ~VmmHandleFactory() override;

        VmmHandleFactory(const VmmHandleFactory&)            = delete;
        VmmHandleFactory& operator=(const VmmHandleFactory&) = delete;

        [[nodiscard]] std::shared_ptr<VmHandle> acquire(const VmTarget& target, std::string& error) override;

    private:
        std::mutex                           _mutex;
        std::vector<std::weak_ptr<VmHandle>> _handles;
    };
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_HANDLE_FACTORY_HPP
