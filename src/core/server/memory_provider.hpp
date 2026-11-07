#ifndef MPFSHOST_CORE_SERVER_MEMORY_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_MEMORY_PROVIDER_HPP

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mpfshost
{
    class MemoryProvider
    {
    public:
        virtual ~MemoryProvider() = default;

        [[nodiscard]] virtual bool readMemory(std::uint32_t           pid,
                                              std::uint64_t           address,
                                              std::uint32_t           size,
                                              std::vector<std::byte>& out,
                                              std::string&            error) = 0;
        [[nodiscard]] virtual bool
        writeMemory(std::uint32_t pid, std::uint64_t address, std::span<const std::byte> data, std::string& error) = 0;
        [[nodiscard]] virtual bool
        virt2Phys(std::uint32_t pid, std::uint64_t va, std::uint64_t& pa, std::string& error) = 0;
    };

    [[nodiscard]] bool isValidMemoryRange(std::uint64_t address, std::uint32_t size);
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_MEMORY_PROVIDER_HPP
