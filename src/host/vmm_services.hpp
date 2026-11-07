#ifndef MPFSHOST_HOST_VMM_SERVICES_HPP
#define MPFSHOST_HOST_VMM_SERVICES_HPP

#pragma once

#include "core/server/service_provider.hpp"

#include <optional>
#include <string>

#include "vmmdll.h"

namespace mpfshost
{
    [[nodiscard]] std::optional<ServiceListResult>
    listVmmServices(VMM_HANDLE handle, const ServiceListRequest& request, LookupStatus& status, std::string& error);
} // namespace mpfshost

#endif // MPFSHOST_HOST_VMM_SERVICES_HPP
