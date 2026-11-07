#ifndef MPFSHOST_CLI_RENDER_HPP
#define MPFSHOST_CLI_RENDER_HPP
#pragma once

#include <cstdint>
#include <string>

namespace mpfshost::cli
{
    [[nodiscard]] const char* moduleKindName(std::uint16_t kind);
    [[nodiscard]] std::string poolTagText(std::uint32_t tag);
    [[nodiscard]] const char* poolTypeName(std::uint8_t type);
    [[nodiscard]] const char* poolSubsegmentName(std::uint8_t subsegment);
    [[nodiscard]] const char* pfnTypeName(std::uint8_t type);
    [[nodiscard]] const char* pfnExtendedTypeName(std::uint8_t type);
    [[nodiscard]] std::string pteFlagNames(std::uint64_t flags);
    [[nodiscard]] const char* pteTypeName(std::uint32_t type);
    [[nodiscard]] std::string vadExFlagNames(std::uint8_t flags);
    [[nodiscard]] std::string pfnFlagNames(std::uint8_t flags);
    [[nodiscard]] std::string hexPointer(std::uint64_t value);
    [[nodiscard]] std::string mapFlagNames(std::uint16_t flags);
    [[nodiscard]] std::string protectName(std::uint32_t protect);
    [[nodiscard]] const char* stateName(std::uint32_t state);
    [[nodiscard]] const char* heapTypeName(std::uint8_t type);
    [[nodiscard]] const char* heapAllocTypeName(std::uint8_t type);
    [[nodiscard]] const char* regionTypeName(std::uint32_t type);
} // namespace mpfshost::cli

#endif // MPFSHOST_CLI_RENDER_HPP
