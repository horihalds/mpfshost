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

Build the configured mpfshost tree, configuring it first when it does not exist yet.

Options:
  --build-dir <dir>  Build tree to build (default: ${DEFAULT_BUILD_DIR}).
  --target <name>    Build only this target, for example mpfshost, mpfshost_tests
                     or guest-probe (default: all targets).
  --jobs <n>         Parallel build jobs (default: nproc).
  --prefix <dir>     Install prefix for the implicit configure step.
  --verbose, -v      Echo each command before running it.
  --help, -h         Show this help and exit.

Relative --build-dir and --prefix paths resolve against the current directory.
EOF
}

build_dir="${DEFAULT_BUILD_DIR}"
target=""
jobs="$(nproc)"
prefix=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir)
            [ "$#" -ge 2 ] && [ -n "$2" ] || die "--build-dir requires a value"
            build_dir="$2"
            shift 2
            ;;
        --target)
            [ "$#" -ge 2 ] && [ -n "$2" ] || die "--target requires a value"
            target="$2"
            shift 2
            ;;
        --jobs)
            [ "$#" -ge 2 ] && [ -n "$2" ] || die "--jobs requires a value"
            jobs="$2"
            shift 2
            ;;
        --prefix)
            [ "$#" -ge 2 ] && [ -n "$2" ] || die "--prefix requires a value"
            prefix="$2"
            shift 2
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

case "${jobs}" in
    ''|*[!0-9]*)
        die "--jobs must be a positive integer, got '${jobs}'"
        ;;
esac
[ "${jobs}" -gt 0 ] || die "--jobs must be a positive integer, got '${jobs}'"

refuse_root "a root-owned build tree breaks catch_discover_tests and leaves root-owned files behind"

require_command cmake "install CMake 3.25 or newer"

if [ ! -f "${build_dir}/CMakeCache.txt" ]; then
    info "no CMake cache in ${build_dir}; configuring it first"
    configure_args=(--build-dir "${build_dir}")
    if [ -n "${prefix}" ]; then
        configure_args+=(--prefix "${prefix}")
    fi
    run "${SCRIPT_DIR}/configure.sh" "${configure_args[@]}"
fi

build_args=(--build "${build_dir}" --parallel "${jobs}")
if [ -n "${target}" ]; then
    build_args+=(--target "${target}")
fi

run cmake "${build_args[@]}"

if [ -z "${target}" ] || [ "${target}" = "mpfshost" ]; then
    bin="${build_dir}/mpfshost"
    [ -f "${bin}" ] || die "build finished but ${bin} is missing"

    missing=()
    for asset in vmm.so leechcore.so info.db; do
        [ -f "${build_dir}/${asset}" ] || missing+=("${asset}")
    done
    if [ "${#missing[@]}" -gt 0 ]; then
        die "runtime assets missing beside ${bin}: ${missing[*]}"
    fi

    info "built ${bin}"
    info "runtime assets present: vmm.so, leechcore.so, info.db"
else
    info "built target ${target} in ${build_dir}"
fi
