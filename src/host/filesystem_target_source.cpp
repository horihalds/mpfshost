#include "host/filesystem_target_source.hpp"

namespace mpfshost
{
    FilesystemTargetSource::FilesystemTargetSource() : _scanner {"/run/libvirt/qemu", "/var/qmp-sockets"} {}

    std::vector<VmTarget> FilesystemTargetSource::scan() const
    {
        return _scanner.scan();
    }
} // namespace mpfshost
