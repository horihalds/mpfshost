#include "core/version.hpp"

namespace mpfshost
{
    std::string_view version() noexcept
    {
        return MPFSHOST_VERSION;
    }
} // namespace mpfshost
