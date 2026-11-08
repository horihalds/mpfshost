#include "core/server/module_lookup_provider.hpp"

namespace mpfshost
{
    const ModuleDescriptor* findModuleAtAddress(const std::vector<ModuleDescriptor>& modules,
                                                std::uint64_t                        address) noexcept
    {
        for (const auto& module : modules)
        {
            if (module.imageSize != 0 && address >= module.base && address - module.base < module.imageSize)
            {
                return &module;
            }
        }
        return nullptr;
    }
} // namespace mpfshost
