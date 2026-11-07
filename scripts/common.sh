# Shared helpers for the configure/build/install entry points. This file is sourced, never executed:
# it sets no strict mode and does no work beyond resolving the paths below.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd -P)"

DEFAULT_BUILD_DIR="build"
DEFAULT_PREFIX="/usr/local"

SCRIPT_NAME="${SCRIPT_NAME:-$(basename "${BASH_SOURCE[1]:-${BASH_SOURCE[0]}}")}"

die()
{
    printf '%s: error: %s\n' "${SCRIPT_NAME}" "$*" >&2
    exit 1
}

warn()
{
    printf '%s: warning: %s\n' "${SCRIPT_NAME}" "$*" >&2
}

info()
{
    printf '%s: %s\n' "${SCRIPT_NAME}" "$*" >&2
}

require_command()
{
    local cmd="$1"
    local hint="${2:-}"
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        if [ -n "${hint}" ]; then
            die "'${cmd}' is required but was not found on PATH; ${hint}"
        fi
        die "'${cmd}' is required but was not found on PATH"
    fi
}

run()
{
    if [ "${MPFSHOST_SCRIPT_VERBOSE:-0}" -eq 1 ]; then
        printf '%s: + %s\n' "${SCRIPT_NAME}" "$*" >&2
    fi
    "$@"
}

cache_value()
{
    local build_dir="$1"
    local var="$2"
    local cache="${build_dir}/CMakeCache.txt"
    [ -f "${cache}" ] || return 0
    sed -n "s/^${var}:[A-Z]*=//p" "${cache}" | head -n 1
}

resolve_path()
{
    case "$1" in
        /*) printf '%s\n' "$1" ;;
        *) printf '%s\n' "${PWD}/$1" ;;
    esac
}

refuse_root()
{
    if [ "$(id -u)" -eq 0 ]; then
        die "refusing to run as root: ${1}"
    fi
}
