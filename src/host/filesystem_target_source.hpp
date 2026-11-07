#ifndef MPFSHOST_HOST_FILESYSTEM_TARGET_SOURCE_HPP
#define MPFSHOST_HOST_FILESYSTEM_TARGET_SOURCE_HPP
#pragma once

#include "core/server/backend.hpp"
#include "core/target/scanner.hpp"

#include <vector>

namespace mpfshost
{
    class FilesystemTargetSource : public TargetSource
    {
    public:
        FilesystemTargetSource();

        [[nodiscard]] std::vector<VmTarget> scan() const override;

    private:
        TargetScanner _scanner;
    };
} // namespace mpfshost

#endif // MPFSHOST_HOST_FILESYSTEM_TARGET_SOURCE_HPP
