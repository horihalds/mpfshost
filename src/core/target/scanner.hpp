#ifndef MPFSHOST_CORE_TARGET_SCANNER_HPP
#define MPFSHOST_CORE_TARGET_SCANNER_HPP
#pragma once

#include "core/target/target.hpp"

#include <filesystem>
#include <vector>

namespace mpfshost
{
    class TargetScanner
    {
    public:
        TargetScanner(std::filesystem::path pidDir, std::filesystem::path qmpDir);

        [[nodiscard]] std::vector<VmTarget> scan() const;

    private:
        std::filesystem::path _pidDir;
        std::filesystem::path _qmpDir;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_TARGET_SCANNER_HPP
