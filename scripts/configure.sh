#!/usr/bin/env bash
set -euo pipefail

SCRIPT_NAME="$(basename "$0")"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck source=scripts/common.sh
source "${SCRIPT_DIR}/common.sh"

usage()
{
    cat <<EOF
Usage: ${SCRIPT_NAME} [options] [-- <cmake args>...]

Configure the mpfshost build tree with the Ninja generator.

Options:
  --build-dir <dir>    Build tree to create or reconfigure (default: ${DEFAULT_BUILD_DIR}).
  --prefix <dir>       Install prefix baked into the systemd unit and mpfshost.pc
                       (default: ${DEFAULT_PREFIX}).
  --build-type <type>  CMAKE_BUILD_TYPE, for example Debug or RelWithDebInfo (default: unset).
  --no-tests           Configure with -DBUILD_TESTING=OFF.
  --fresh              Discard the existing cache first (cmake --fresh).
  --verbose, -v        Echo each command before running it.
  --help, -h           Show this help and exit.
  -- <args>...         Pass the remaining arguments to cmake verbatim.

Relative --build-dir and --prefix paths resolve against the current directory.
EOF
}

build_dir="${DEFAULT_BUILD_DIR}"
prefix="${DEFAULT_PREFIX}"
build_type=""
tests=1
fresh=0
extra=()

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir)
            [ "$#" -ge 2 ] && [ -n "$2" ] || die "--build-dir requires a value"
            build_dir="$2"
            shift 2
            ;;
        --prefix)
            [ "$#" -ge 2 ] && [ -n "$2" ] || die "--prefix requires a value"
            prefix="$2"
            shift 2
            ;;
        --build-type)
            [ "$#" -ge 2 ] && [ -n "$2" ] || die "--build-type requires a value"
            build_type="$2"
            shift 2
            ;;
        --no-tests)
            tests=0
            shift
            ;;
        --fresh)
            fresh=1
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
        --)
            shift
            extra=("$@")
            break
            ;;
        *)
            usage >&2
            die "unknown option '$1'"
            ;;
    esac
done

prefix="$(resolve_path "${prefix}")"

require_command cmake "install CMake 3.25 or newer"
require_command ninja "install Ninja, the supported generator"

[ -f "${REPO_ROOT}/CMakeLists.txt" ] || die "CMakeLists.txt not found in ${REPO_ROOT}"

for asset in vmm.so leechcore.so info.db; do
    [ -f "${REPO_ROOT}/MemProcFS/${asset}" ] || die "missing vendored MemProcFS asset: ${REPO_ROOT}/MemProcFS/${asset}"
done

args=(-S "${REPO_ROOT}" -B "${build_dir}" -DCMAKE_INSTALL_PREFIX="${prefix}")

# An existing tree already knows its generator; only a cache-less or freshly discarded tree gets -G.
if [ ! -f "${build_dir}/CMakeCache.txt" ] || [ "${fresh}" -eq 1 ]; then
    args+=(-G Ninja)
fi
if [ -n "${build_type}" ]; then
    args+=(-DCMAKE_BUILD_TYPE="${build_type}")
fi
if [ "${tests}" -eq 0 ]; then
    args+=(-DBUILD_TESTING=OFF)
fi
if [ "${fresh}" -eq 1 ]; then
    args+=(--fresh)
fi
args+=("${extra[@]}")

run cmake "${args[@]}"

info "configured ${build_dir} with prefix ${prefix}"
if [ -n "${build_type}" ]; then
    info "build type: ${build_type}"
fi
if [ "${tests}" -eq 0 ]; then
    info "tests disabled"
fi
info "next: ${SCRIPT_DIR}/build.sh --build-dir ${build_dir}"
