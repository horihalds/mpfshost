#!/usr/bin/env bash
set -euo pipefail

SCRIPT_NAME="$(basename "$0")"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck source=scripts/common.sh
source "${SCRIPT_DIR}/common.sh"

usage()
{
    cat <<EOF
Usage: ${SCRIPT_NAME} [options]

Install mpfshost and its runtime assets, then enable and restart the service.

Options:
  --prefix <dir>     Install prefix (default: the tree's configured prefix, else ${DEFAULT_PREFIX}).
  --build-dir <dir>  Build tree to install from (default: ${DEFAULT_BUILD_DIR}).
  --skip-build       Install the existing tree without rebuilding it.
  --no-service       Skip daemon-reload, enable, restart and status.
  --add-user         Add the invoking user to group mpfshost (sudo usermod -aG).
  --dry-run          Print the privileged commands instead of running them; nothing is written.
  --verbose, -v      Echo each command before running it.
  --help, -h         Show this help and exit.

The prefix must match the one the tree was configured with: the systemd unit and
mpfshost.pc bake it at configure time, so a mismatch triggers a reconfigure and rebuild.
Only cmake --install, the usermod call and the systemctl calls run through sudo; the
build never does. After a service restart the installed client is run as your user; if
it cannot reach the socket the script reports the reason and exits non-zero, leaving the
installation itself intact.

Relative --build-dir and --prefix paths resolve against the current directory.
EOF
}

run_privileged()
{
    if [ "${dry_run}" -eq 1 ]; then
        printf '%s: would run: sudo %s\n' "${SCRIPT_NAME}" "$*" >&2
        return 0
    fi
    run sudo "$@"
}

verify_install()
{
    local bin="${prefix}/bin/mpfshost"
    local version dynamic runpath_line ldd_out lib

    [ -x "${bin}" ] || die "install finished but ${bin} is missing"

    version="$("${bin}" --version)" || die "${bin} --version failed"
    [ -n "${version}" ] || die "${bin} --version printed nothing"
    info "installed binary reports version ${version}"

    if command -v readelf >/dev/null 2>&1; then
        dynamic="$(readelf -d "${bin}")"
    elif command -v objdump >/dev/null 2>&1; then
        dynamic="$(objdump -p "${bin}")"
    else
        warn "neither readelf nor objdump was found; skipping the RUNPATH check"
        dynamic=""
    fi

    if [ -n "${dynamic}" ]; then
        runpath_line="$(printf '%s\n' "${dynamic}" | grep -E '\((RUNPATH|RPATH)\)' || true)"
        case "${runpath_line}" in
            *'$ORIGIN'*) info "RUNPATH contains \$ORIGIN" ;;
            *) die "${bin} has no \$ORIGIN in RUNPATH/RPATH: ${runpath_line:-none}" ;;
        esac
    fi

    if command -v ldd >/dev/null 2>&1; then
        ldd_out="$(ldd "${bin}" || true)"
        for lib in vmm.so leechcore.so; do
            if printf '%s\n' "${ldd_out}" | grep -q "${lib}.*not found"; then
                die "ldd cannot resolve ${lib} for ${bin}"
            fi
            if ! printf '%s\n' "${ldd_out}" | grep -q "${lib} => /"; then
                die "ldd does not report ${lib} for ${bin}"
            fi
        done
        info "ldd resolves vmm.so and leechcore.so"
    else
        warn "ldd was not found; skipping the shared-library resolution check"
    fi
}

group_exists()
{
    if command -v getent >/dev/null 2>&1; then
        getent group "$1" >/dev/null 2>&1
    else
        grep -qx "$1:.*" /etc/group
    fi
}

# The unit runs with the default access policy, which enforces group mpfshost; without the group the
# host refuses to start, so it has to exist before the service is restarted.
ensure_group()
{
    local group="mpfshost"
    local user

    if ! group_exists "${group}"; then
        if ! run_privileged groupadd --system "${group}"; then
            die "could not create group ${group}; the host refuses to start without it"
        fi
        info "created system group ${group}"
    fi

    user="$(id -un)"
    if ! id -nG "${user}" | tr ' ' '\n' | grep -qx "${group}"; then
        info "user ${user} is not a member of group ${group}; run: sudo usermod -aG ${group} ${user}"
        info "new group memberships apply to new sessions only; log in again or run 'newgrp ${group}'"
    fi
}

# Only affects the account database; the invoking session keeps its login-time group list, which
# check_client_access() reports on. Membership already recorded in /etc/group is left untouched.
add_user_to_group()
{
    local group="mpfshost"
    local user

    user="$(id -un)"
    if id -nG "${user}" | tr ' ' '\n' | grep -qx "${group}"; then
        info "user ${user} is already a member of group ${group}"
        return 0
    fi

    if ! run_privileged usermod -aG "${group}" "${user}"; then
        warn "could not add ${user} to group ${group}; run: sudo usermod -aG ${group} ${user}"
        return 0
    fi
    if [ "${dry_run}" -eq 1 ]; then
        return 0
    fi
    info "added ${user} to group ${group}; run 'newgrp ${group}' or log in again to use it"
}

# The unit starts asynchronously, so the installed client is retried before the failure is reported.
check_client_access()
{
    local bin="${prefix}/bin/mpfshost"
    local socket_path="/run/mpfshost.sock"
    local attempt output

    if [ "${dry_run}" -eq 1 ]; then
        info "would check access with: ${bin} --socket ${socket_path} --client list-targets"
        return 0
    fi

    for attempt in $(seq 1 10); do
        if output="$("${bin}" --socket "${socket_path}" --client list-targets 2>&1)"; then
            info "client can reach ${socket_path} as $(id -un)"
            return 0
        fi
        sleep 0.2
    done

    warn "client cannot reach ${socket_path} as $(id -un): ${output}"
    warn "log in again or run 'newgrp mpfshost', or rerun this script with --add-user"
    return 1
}

handle_service()
{
    local unit="${build_dir}/mpfshost.service"

    if ! command -v systemctl >/dev/null 2>&1; then
        warn "systemctl was not found; skipping service activation"
        return 0
    fi

    run_privileged systemctl daemon-reload

    if [ "${dry_run}" -eq 1 ]; then
        info "would copy ${unit} to /etc/systemd/system if systemctl cannot see the unit"
    elif ! systemctl cat mpfshost >/dev/null 2>&1; then
        if [ -f "${unit}" ]; then
            info "systemctl does not see mpfshost; installing ${unit} into /etc/systemd/system"
            run_privileged install -m 0644 "${unit}" /etc/systemd/system/mpfshost.service
            run_privileged systemctl daemon-reload
        else
            warn "rendered unit ${unit} is missing; systemctl will not find mpfshost"
        fi
    fi

    run_privileged systemctl enable mpfshost

    info "restarting mpfshost drops live client sessions and cached VMM_HANDLEs"
    run_privileged systemctl restart mpfshost

    if [ "${dry_run}" -eq 1 ]; then
        info "would run: systemctl --no-pager status mpfshost"
    else
        run systemctl --no-pager status mpfshost || true
    fi
}

print_pkg_config_hint()
{
    local libdir="$1"
    local pkgconfig_dir="${prefix}/${libdir}/pkgconfig"
    local pc_path

    if ! command -v pkg-config >/dev/null 2>&1; then
        warn "pkg-config was not found; skipping the discovery hint"
        return 0
    fi

    pc_path="$(pkg-config --variable pc_path pkg-config)"
    case ":${pc_path}:" in
        *":${pkgconfig_dir}:"*) ;;
        *) info "pkg-config does not search ${pkgconfig_dir}; use PKG_CONFIG_PATH=${pkgconfig_dir}" ;;
    esac
}

build_dir="${DEFAULT_BUILD_DIR}"
requested_prefix=""
skip_build=0
service=1
add_user=0
dry_run=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix)
            [ "$#" -ge 2 ] && [ -n "$2" ] || die "--prefix requires a value"
            requested_prefix="$2"
            shift 2
            ;;
        --build-dir)
            [ "$#" -ge 2 ] && [ -n "$2" ] || die "--build-dir requires a value"
            build_dir="$2"
            shift 2
            ;;
        --skip-build)
            skip_build=1
            shift
            ;;
        --no-service)
            service=0
            shift
            ;;
        --add-user)
            add_user=1
            shift
            ;;
        --dry-run)
            dry_run=1
            shift
            ;;
        --verbose|-v)
            export MPFSHOST_SCRIPT_VERBOSE=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            usage >&2
            die "unknown option '$1'"
            ;;
    esac
done

refuse_root "sudo is used per command, and the build must stay owned by your user: the test binary \
refuses to run as root"

require_command cmake "install CMake 3.25 or newer"
if [ "${dry_run}" -eq 0 ]; then
    require_command sudo "the install and the systemctl calls need root"
fi

if [ "${skip_build}" -eq 0 ]; then
    if [ "${dry_run}" -eq 1 ]; then
        info "would build ${build_dir} first"
    else
        run "${SCRIPT_DIR}/build.sh" --build-dir "${build_dir}"
    fi
fi

configured_prefix="$(cache_value "${build_dir}" CMAKE_INSTALL_PREFIX)"
configured_prefix="${configured_prefix%/}"

if [ -n "${requested_prefix}" ]; then
    prefix="$(resolve_path "${requested_prefix}")"
else
    prefix="${configured_prefix:-${DEFAULT_PREFIX}}"
fi
prefix="${prefix%/}"
[ -n "${prefix}" ] || prefix="/"

if [ -z "${configured_prefix}" ]; then
    if [ "${dry_run}" -eq 0 ]; then
        die "no CMake cache in ${build_dir}; run scripts/configure.sh first or drop --skip-build"
    fi
    info "no CMake cache in ${build_dir}; a real run would configure it first"
elif [ "${configured_prefix}" != "${prefix}" ]; then
    info "configured prefix ${configured_prefix} differs from ${prefix}; reconfiguring and rebuilding"
    if [ "${dry_run}" -eq 0 ]; then
        run "${SCRIPT_DIR}/configure.sh" --build-dir "${build_dir}" --prefix "${prefix}"
        run "${SCRIPT_DIR}/build.sh" --build-dir "${build_dir}"
    fi
fi

run_privileged cmake --install "${build_dir}"

if [ "${dry_run}" -eq 1 ]; then
    info "would verify ${prefix}/bin/mpfshost --version, RUNPATH and shared-library resolution"
else
    verify_install
fi

ensure_group
if [ "${add_user}" -eq 1 ]; then
    add_user_to_group
fi

access_ok=1
if [ "${service}" -eq 1 ]; then
    handle_service
    if ! check_client_access; then
        access_ok=0
    fi
else
    info "service activation skipped (--no-service)"
fi

libdir="$(cache_value "${build_dir}" CMAKE_INSTALL_LIBDIR)"
print_pkg_config_hint "${libdir:-lib}"

if [ "${access_ok}" -eq 0 ]; then
    die "the installation is intact but client access to the socket failed; see the diagnosis above"
fi

info "follow the host log with: journalctl -u mpfshost -f"
