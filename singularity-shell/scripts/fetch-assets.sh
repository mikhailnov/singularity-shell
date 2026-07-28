#!/usr/bin/env bash
# fetch-assets.sh — acquire SingularityApp web assets from the official snap
# without snapd. See qt-tz.md sections 3.5, 6.8, 6.10.
#
# Subcommands:
#   fetch-assets.sh latest  <destdir>            resolve+download+verify+extract
#   fetch-assets.sh extract <file.snap> <destdir>  extraction-only (caller verified)
#
# Exit codes: 1=network, 2=hash mismatch, 3=missing tool, 4=bad API response,
#             5=bad arguments, 6=extraction failure.
set -euo pipefail

SNAP_NAME="singularityapp"
API_URL="https://api.snapcraft.io/v2/snaps/info/${SNAP_NAME}"

die()  { echo "fetch-assets: error: $*" >&2; exit "${2:-1}"; }
need() { command -v "$1" >/dev/null 2>&1 || die "required tool not found: $1" 3; }

# Global temp-dir cleanup (single EXIT trap; RETURN traps are not
# function-scoped and misfire under `set -u`).
TMPDIRS=()
cleanup() { for d in ${TMPDIRS[@]+"${TMPDIRS[@]}"}; do rm -rf "$d"; done; }
trap cleanup EXIT
maketmp() { local d; d="$(mktemp -d)"; TMPDIRS+=("$d"); echo "$d"; }

# Map host CPU arch to snap arch label.
snap_arch() {
    case "$(uname -m)" in
        x86_64)  echo amd64 ;;
        aarch64) echo arm64 ;;
        *)       die "unsupported host arch: $(uname -m)" 4 ;;
    esac
}

# asar-extract lives next to the script (dev tree) or in PATH (installed).
find_asar_extract() {
    local self_dir
    self_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    for candidate in \
        "${self_dir}/../build/asar-extract" \
        "${self_dir}/asar-extract" \
        "$(command -v asar-extract || true)"; do
        [[ -n "${candidate}" && -x "${candidate}" ]] && { echo "${candidate}"; return; }
    done
    die "asar-extract binary not found (build the project first)" 3
}

# extract_asar_tree <snap> <dest_version_dir>
extract_asar_tree() {
    local snap="$1" dest="$2"
    need unsquashfs
    local asar_tool; asar_tool="$(find_asar_extract)"
    local tmp; tmp="$(maketmp)"

    # Pull only the asar out of the squashfs (fast: no full unpack).
    unsquashfs -f -d "${tmp}/sq" "${snap}" 'resources/app.asar' >/dev/null \
        || die "unsquashfs failed on ${snap}" 6
    [[ -f "${tmp}/sq/resources/app.asar" ]] \
        || die "resources/app.asar not found inside snap" 6

    mkdir -p "${dest}"
    "${asar_tool}" "${tmp}/sq/resources/app.asar" "${dest}" >/dev/null \
        || die "asar-extract failed" 6
    [[ -s "${dest}/build/index.html" ]] \
        || die "extraction sanity check failed: build/index.html missing" 6
}

# write_manifest <dest_version_dir> <version> <revision> <sha3-384> <source_url>
write_manifest() {
    local dest="$1" version="$2" revision="$3" sha="$4" url="$5"
    cat > "${dest}/manifest.json" <<EOF
{
  "name": "${SNAP_NAME}",
  "version": "${version}",
  "revision": ${revision},
  "sha3_384": "${sha}",
  "source": "${url}",
  "fetchedAt": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
}

# switch_current <assets_root> <version>
switch_current() {
    local root="$1" version="$2"
    ln -sfn "${root}/${version}" "${root}/current.new"
    mv -T "${root}/current.new" "${root}/current"
}

cmd_latest() {
    local destdir="${1:?usage: fetch-assets.sh latest <destdir>}"
    need curl; need jq; need openssl

    local arch; arch="$(snap_arch)"
    local info; info="$(curl -fsS -H 'Snap-Device-Series: 16' "${API_URL}")" \
        || die "Snap Store API request failed" 1

    local entry; entry="$(jq -c --arg arch "${arch}" '
        [."channel-map"[]
         | select(.channel.track=="latest" and .channel.risk=="stable"
                  and .channel.architecture==$arch)][0] // empty' <<<"${info}")"
    [[ -n "${entry}" ]] || die "no latest/stable build for arch ${arch}" 4

    local version revision url sha
    version="$(jq -r .version <<<"${entry}")"
    revision="$(jq -r .revision <<<"${entry}")"
    url="$(jq -r .download.url <<<"${entry}")"
    sha="$(jq -r '.download["sha3-384"]' <<<"${entry}")"
    [[ "${version}" != "null" && "${url}" != "null" && "${sha}" != "null" ]] \
        || die "incomplete API entry" 4

    local assets_root="${destdir}/assets"
    local dest="${assets_root}/${version}-r${revision}"
    if [[ -f "${dest}/manifest.json" ]]; then
        echo "fetch-assets: ${version}-r${revision} already present, nothing to do"
        switch_current "${assets_root}" "$(basename "${dest}")"
        return 0
    fi

    local tmpd; tmpd="$(maketmp)"
    echo "fetch-assets: downloading ${SNAP_NAME} ${version} (rev ${revision})..."
    curl -fsSL --retry 3 -o "${tmpd}/app.snap" "${url}" \
        || die "download failed" 1

    echo "fetch-assets: verifying sha3-384..."
    local actual; actual="$(openssl dgst -sha3-384 -r "${tmpd}/app.snap" | awk '{print $1}')"
    [[ "${actual}" == "${sha}" ]] \
        || die "sha3-384 mismatch: expected ${sha}, got ${actual}" 2

    extract_asar_tree "${tmpd}/app.snap" "${dest}"
    write_manifest "${dest}" "${version}" "${revision}" "${sha}" "${url}"
    switch_current "${assets_root}" "$(basename "${dest}")"
    echo "fetch-assets: staged ${version}-r${revision} at ${dest}"
}

cmd_extract() {
    local snap="${1:?usage: fetch-assets.sh extract <file.snap> <destdir>}"
    local destdir="${2:?usage: fetch-assets.sh extract <file.snap> <destdir>}"
    # Caller (UpdateController) already verified the hash and chose the
    # versioned destination directory name.
    mkdir -p "${destdir}"
    extract_asar_tree "${snap}" "${destdir}"
}

case "${1:-}" in
    latest)  shift; cmd_latest "$@" ;;
    extract) shift; cmd_extract "$@" ;;
    *) echo "usage: fetch-assets.sh {latest <destdir>|extract <file.snap> <destdir>}" >&2; exit 5 ;;
esac
