#ifndef MPFSHOST_CORE_UTIL_SIGNATURE_HPP
#define MPFSHOST_CORE_UTIL_SIGNATURE_HPP
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace mpfshost
{
    // A byte pattern written the way IDA spells signatures: two hex digits per byte, `?` for one wildcard byte
    // and `u4` for four wildcard bytes whose little-endian value is captured. Exactly one `u4` is allowed.
    struct SignaturePattern
    {
        std::vector<std::byte> bytes;
        std::vector<std::byte> mask; // 0xFF matches exactly, 0x00 is a wildcard
        std::size_t            captureOffset {};
        bool                   hasCapture {};
    };

    struct SignatureMatch
    {
        std::size_t   offset {};
        std::uint32_t captured {};
    };

    [[nodiscard]] std::optional<SignaturePattern> parseSignature(std::string_view text);

    // Returns the offset of the first match and the captured `u4` value when the pattern declares one.
    [[nodiscard]] std::optional<SignatureMatch> findSignature(std::span<const std::byte> data,
                                                              const SignaturePattern&    pattern);
} // namespace mpfshost

#endif // MPFSHOST_CORE_UTIL_SIGNATURE_HPP
