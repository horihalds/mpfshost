#!/usr/bin/env bash
set -euo pipefail

SCRIPT_NAME="$(basename "$0")"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck source=scripts/common.sh
source "${SCRIPT_DIR}/common.sh"

RELEASES_REPO="ufrisk/MemProcFS"
API_ROOT="https://api.github.com/repos/${RELEASES_REPO}/releases"

# The files mpfshost actually loads, plus the licences covering them. Everything else the upstream
# Linux archive ships belongs to the standalone memprocfs/pcileech tools, to the Python bindings or
# to optional components this host never initialises (plugins, YARA, gRPC, non-QEMU devices), so it
# is deliberately not vendored. Keep this list in step with the CMake install rules and AGENTS.md.
# `libpdbcrust` is missing here on purpose: MemProcFS ships one build per OpenSSL generation and
# the matching one is appended once the local OpenSSL is known.
MANIFEST=(
    vmm.so
    leechcore.so
    leechcore_device_qemu.so
    libMSCompression.so
    info.db
    vmmdll.h
    leechcore.h
    LICENSE.txt
    license_info_all.txt
    license_libmscompression.txt
    license_libpdbcrust.txt
    license_lz4.txt
)

# Upstream ships both directories empty. `Symbols` is the PDB cache; `VMMDLL_InitializePlugins`
# reports a missing `plugins` directory and the host treats that failure as fatal, so the directory
# stays even though no plugin payload is vendored. Tracked through their .gitignore files.
EXTRA_DIRS=(Symbols plugins)

usage()
{
    cat <<EOF
Usage: ${SCRIPT_NAME} [options]

Download the latest MemProcFS Linux release from ${RELEASES_REPO} and vendor only the files
mpfshost needs into the MemProcFS directory.

Options:
  --dest <dir>      Destination directory (default: ${REPO_ROOT}/MemProcFS).
  --version <tag>   Fetch a specific release tag instead of the latest one (for example v5.19).
  --arch <arch>     Release architecture: linux_x64 or linux_aarch64
                    (default: derived from uname -m).
  --keep-extra      Keep files in the destination that are not part of the manifest.
  --dry-run         Resolve the release and print the plan without downloading or writing anything.
  --list            Print the vendored file list and exit.
  --verbose, -v     Echo each command before running it.
  --help, -h        Show this help and exit.

Files outside the manifest are removed from the destination unless --keep-extra is given, so the
directory stays exactly the curated subset; VERSION and .gitignore files are always kept.

Downloads are kept in ./tmp/fetch-memprocfs and reused on later runs; delete the archive there to
fetch it again. Set GITHUB_TOKEN to lift the anonymous GitHub API rate limit.
EOF
}

detect_arch()
{
    case "$(uname -m)" in
        x86_64|amd64)
            printf 'linux_x64\n'
            ;;
        aarch64|arm64)
            printf 'linux_aarch64\n'
            ;;
        *)
            die "unsupported architecture '$(uname -m)'; pass --arch linux_x64 or --arch linux_aarch64"
            ;;
    esac
}

# MemProcFS dlopens libpdbcrust.so first and falls back to libpdbcrust2.so. The two released builds
# are identical apart from the OpenSSL they link, so only the one this machine can load is vendored.
select_pdbcrust()
{
    local cache=""
    if command -v ldconfig >/dev/null 2>&1; then
        cache="$(ldconfig -p 2>/dev/null || true)"
    fi
    if [[ "${cache}" == *"libssl.so.3"* ]]; then
        printf 'libpdbcrust.so\n'
    elif [[ "${cache}" == *"libssl.so.1.1"* ]]; then
        printf 'libpdbcrust2.so\n'
    else
        printf 'libpdbcrust.so\n'
    fi
}

is_manifest()
{
    local wanted="$1"
    local entry
    if [ "${wanted}" = "VERSION" ] || [ "$(basename "${wanted}")" = ".gitignore" ]; then
        return 0
    fi
    for entry in "${MANIFEST[@]}"; do
        if [ "${entry}" = "${wanted}" ]; then
            return 0
        fi
    done
    return 1
}

json_values()
{
    local key="$1"
    tr ',' '\n' < "${work_dir}/release.json" |
        sed -n "s/.*\"${key}\": *\"\\([^\"]*\\)\".*/\\1/p"
}

dest="${REPO_ROOT}/MemProcFS"
version=""
arch=""
keep_extra=0
dry_run=0
list_only=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --dest)
            [ "$#" -ge 2 ] && [ -n "$2" ] || die "--dest requires a value"
            dest="$2"
            shift 2
            ;;
        --version)
            [ "$#" -ge 2 ] && [ -n "$2" ] || die "--version requires a value"
            version="$2"
            shift 2
            ;;
        --arch)
            [ "$#" -ge 2 ] && [ -n "$2" ] || die "--arch requires a value"
            arch="$2"
            shift 2
            ;;
        --keep-extra)
            keep_extra=1
            shift
            ;;
        --dry-run)
            dry_run=1
            shift
            ;;
        --list)
            list_only=1
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

refuse_root "the vendored tree must stay owned by your user"

dest="$(resolve_path "${dest}")"
dest="${dest%/}"

if [ -z "${arch}" ]; then
    arch="$(detect_arch)"
fi
case "${arch}" in
    linux_x64|linux_aarch64) ;;
    *) die "unsupported --arch '${arch}'; expected linux_x64 or linux_aarch64" ;;
esac

pdbcrust="$(select_pdbcrust)"
MANIFEST+=("${pdbcrust}")

if [ "${list_only}" -eq 1 ]; then
    printf '%s\n' "${MANIFEST[@]}"
    exit 0
fi

require_command curl "install curl to download the release"
require_command tar "install tar to unpack the release"
require_command install "install coreutils for the file copy"
require_command sha256sum "install coreutils for the archive checksum"

work_dir="${REPO_ROOT}/tmp/fetch-memprocfs"
run mkdir -p "${work_dir}"

if [ -n "${version}" ]; then
    release_url="${API_ROOT}/tags/${version}"
else
    release_url="${API_ROOT}/latest"
fi

curl_args=(--fail --silent --show-error --location --retry 3 --proto '=https'
    -H 'Accept: application/vnd.github+json'
    -o "${work_dir}/release.json")
if [ -n "${GITHUB_TOKEN:-}" ]; then
    curl_args+=(-H "Authorization: Bearer ${GITHUB_TOKEN}")
fi
run curl "${curl_args[@]}" "${release_url}" || die "could not query ${release_url}"

mapfile -t release_tags < <(json_values tag_name)
release_tag="${release_tags[0]:-}"
[ -n "${release_tag}" ] || die "could not read the release tag from ${release_url}"

asset_url=""
while IFS= read -r candidate; do
    asset_url="${candidate}"
done < <(json_values browser_download_url | grep -E "(^|[-_])${arch}-[0-9]+\.tar\.gz$" | sort -V)
if [ -z "${asset_url}" ]; then
    available="$(json_values browser_download_url | sed 's#.*/##' | tr '\n' ' ')"
    die "release ${release_tag} has no ${arch} tar.gz asset (available: ${available:-none})"
fi
asset_name="$(basename "${asset_url}")"

if [ "${dry_run}" -eq 1 ]; then
    info "release:  ${release_tag}"
    info "asset:    ${asset_name}"
    info "source:   ${asset_url}"
    info "dest:     ${dest}"
    info "pdbcrust: ${pdbcrust}"
    printf '%s\n' "${MANIFEST[@]}"
    exit 0
fi

archive="${work_dir}/${asset_name}"
if [ -s "${archive}" ]; then
    info "using cached ${asset_name}"
else
    info "downloading ${asset_name} (${release_tag})"
    run curl --fail --silent --show-error --location --retry 3 --proto '=https' \
        -o "${archive}.part" "${asset_url}" || die "could not download ${asset_url}"
    mv "${archive}.part" "${archive}"
fi

staging="${work_dir}/staging"
rm -rf "${staging}"
run mkdir -p "${staging}"

tar_members=()
while IFS= read -r member; do
    tar_members+=("${member}")
done < <(tar tzf "${archive}" | sed -e 's#^\./##' -e '/^$/d')

selected=()
missing=()
for name in "${MANIFEST[@]}"; do
    found=0
    for member in "${tar_members[@]}"; do
        if [ "${member}" = "${name}" ]; then
            found=1
            break
        fi
    done
    if [ "${found}" -eq 0 ]; then
        missing+=("${name}")
    fi
    selected+=("./${name}")
done
if [ "${#missing[@]}" -gt 0 ]; then
    die "release ${release_tag} is missing: ${missing[*]}"
fi

run tar -xzf "${archive}" -C "${staging}" --no-same-owner -- "${selected[@]}" ||
    die "could not unpack ${asset_name}"

run mkdir -p "${dest}"
for name in "${MANIFEST[@]}"; do
    run install -m 0644 "${staging}/${name}" "${dest}/${name}"
done

if [ "${keep_extra}" -eq 0 ]; then
    while IFS= read -r -d '' path; do
        relative="${path#"${dest}"/}"
        if ! is_manifest "${relative}"; then
            run rm -f -- "${path}"
        fi
    done < <(find "${dest}" -type f -print0)
    run find "${dest}" -mindepth 1 -depth -type d -empty -delete
fi

for dir in "${EXTRA_DIRS[@]}"; do
    run mkdir -p "${dest}/${dir}"
done

read -r digest _ < <(sha256sum "${archive}")
{
    printf 'tag=%s\n' "${release_tag}"
    printf 'asset=%s\n' "${asset_name}"
    printf 'sha256=%s\n' "${digest}"
} > "${dest}/VERSION"
run chmod 0644 "${dest}/VERSION"

rm -rf "${staging}"

info "vendored ${release_tag} into ${dest}"
info "asset ${asset_name} (sha256 ${digest})"
info "pdbcrust ${pdbcrust}"
