#ifndef MPFSHOST_CORE_SERVER_PROCESS_INFO_PROVIDER_HPP
#define MPFSHOST_CORE_SERVER_PROCESS_INFO_PROVIDER_HPP
#pragma once

#include "core/ipc/message.hpp"
#include "core/server/backend.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace mpfshost
{
    // A CR3 register may carry PCID/PCD/PWT bits in its low twelve bits; the page-directory base never does.
    [[nodiscard]] constexpr std::uint64_t canonicalDtb(std::uint64_t dtb) noexcept
    {
        return dtb & ~std::uint64_t {0xFFF};
    }

    class ProcessInfoProvider
    {
    public:
        virtual ~ProcessInfoProvider() = default;

        [[nodiscard]] virtual std::optional<ProcessInfoResponse>
        processInfo(const ProcessInfoRequest& request, LookupStatus& status, std::string& error) = 0;

        [[nodiscard]] virtual std::optional<ProcessByDtbResponse>
        processByDtb(const ProcessByDtbRequest& request, LookupStatus& status, std::string& error) = 0;
    };
} // namespace mpfshost

#endif // MPFSHOST_CORE_SERVER_PROCESS_INFO_PROVIDER_HPP
