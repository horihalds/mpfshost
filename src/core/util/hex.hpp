#ifndef MPFSHOST_CORE_UTIL_HEX_HPP
#define MPFSHOST_CORE_UTIL_HEX_HPP

#pragma once

#include "core/ipc/message.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mpfshost
{
    [[nodiscard]] std::optional<std::uint64_t> parseHexU64(std::string_view text);

    // Accepts "deadbeef", "DE AD BE EF" and common separators; rejects an odd digit count or a non-hex character.
    [[nodiscard]] std::optional<std::vector<std::byte>> parseHexBytes(std::string_view text);

    [[nodiscard]] std::string formatHexDump(std::span<const std::byte> data, std::uint64_t baseAddress);

    // Accepts "de ad be ef", "deadbeef" and "0xDE,0xAD" alike, with "??" (or "?") as a wildcard byte. A
    // token has to name whole bytes, so an odd digit count is rejected.
    [[nodiscard]] std::optional<SearchTerm> parseHexPattern(std::string_view text);

    // A literal text pattern: every byte has to match.
    [[nodiscard]] SearchTerm makeSearchTerm(std::string_view text);

    // VMMDLL_MemSearch compares a term's raw pattern bytes against zero and fails the whole call when they are
    // all zero, wildcards included, instead of reporting no hits.
    [[nodiscard]] bool isSearchableTerm(const SearchTerm& term) noexcept;
} // namespace mpfshost

#endif // MPFSHOST_CORE_UTIL_HEX_HPP
