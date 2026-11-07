#ifndef MPFSHOST_CORE_SERVER_BACKEND_HPP
#define MPFSHOST_CORE_SERVER_BACKEND_HPP
#pragma once

#include "core/target/target.hpp"

#include <memory>
#include <string>
#include <vector>

namespace mpfshost
{
    class TargetSource
    {
    public:
        virtual ~TargetSource()                                  = default;
        [[nodiscard]] virtual std::vector<VmTarget> scan() const = 0;
    };

    class VmHandle
    {
    public:
        virtual ~VmHandle() = default;
    };

    // A lookup that resolves a process, module or symbol by name distinguishes "nothing carries that name" from a
    // real failure, so the session can answer ErrorCode::NotFound instead of the generic Internal.
    enum class LookupStatus
    {
        Ok,
        NotFound,
        Failed,
    };

    class VmBackend
    {
    public:
        virtual ~VmBackend()                                                                                = default;
        [[nodiscard]] virtual std::shared_ptr<VmHandle> acquire(const VmTarget& target, std::string& error) = 0;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_BACKEND_HPP
