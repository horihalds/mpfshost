#ifndef MPFSHOST_CLI_RUN_CLIENT_HPP
#define MPFSHOST_CLI_RUN_CLIENT_HPP
#pragma once

#include "cli/options.hpp"

namespace mpfshost::cli
{
    // Returns the process exit code: 0 on success, 1 when the request failed.
    [[nodiscard]] int runClient(const ClientOptions& options);
} // namespace mpfshost::cli

#endif // MPFSHOST_CLI_RUN_CLIENT_HPP
