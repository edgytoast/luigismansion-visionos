#!/usr/bin/env bash
# Builds Luigi's Mansion VR for Apple Vision Pro: the game (LuigisMansionVisionGame.framework),
# then the SwiftUI app around it (xcodegen + xcodebuild), and optionally installs
# it on the paired headset.
#
#   visionos/scripts/build-visionos.sh --team TEAMID [--bundle-id ID] [--debug]
#                                      [--install [--device UDID]] [--game-only]
#   visionos/scripts/build-visionos.sh --simulator [--debug] [--game-only]
#
# --simulator builds for the visionOS Simulator, unsigned (no team needed).
# The bundle ID defaults to dev.lmvr.vision.<TEAMID>: a bare ID may already be
# registered to another team, and automatic signing then falls back to a wildcard
# profile without the memory entitlement.
set -euo pipefail

root="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
visionos="${root}/visionos"
team=""
bundle_id=""
config="RelWithDebInfo"
xcode_config="Release"
install=0
device=""
game_only=0
simulator=0
stub=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --team) team="${2:?}"; shift 2 ;;
        --bundle-id) bundle_id="${2:?}"; shift 2 ;;
        --debug) config="Debug"; xcode_config="Debug"; shift ;;
        --install) install=1; shift ;;
        --device) device="${2:?}"; shift 2 ;;
        --game-only) game_only=1; shift ;;
        --simulator) simulator=1; shift ;;
        -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

# CMake's Xcode lookup breaks when the Xcode path has a space in it; keep the
# active Xcode at /Applications/Xcode.app.
if [[ -z "${DEVELOPER_DIR:-}" && -d /Applications/Xcode.app/Contents/Developer ]]; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi
for tool in xcodegen xcrun; do
    command -v "${tool}" >/dev/null || { echo "ERROR: ${tool} not found (brew install ${tool})" >&2; exit 1; }
done
if [[ ${simulator} -eq 1 ]]; then flavour="xrsimulator"; else flavour="visionos"; fi
build="${root}/.scratch/build-${flavour}-${config}"

# 1. The game framework.
# The base tree's CMake build (the archive plus this port's patches), cross-compiled for visionOS,
# with visionos/cmake/framework.cmake adding the framework target.
#
# The game module is recompiled from the player's own main.dol at build time, on this Mac: set
# LMVR_DISC to the disc image (.iso/.gcm; GLME01 USA or GLMP01 Europe), or LMVR_DOL to a main.dol
# with LMVR_DOL_VERSION. Without either, the framework builds with a stand-in module (the VR
# pipeline and the menu run; the game doesn't). Everything made from the disc stays in .scratch.
build_game() {
    # The game's source: LMVR_BASE_DIR if set; else this repository itself when it carries the
    # source (the public release); else the development base tree (apply-base.sh's .scratch/base).
    local base="${LMVR_BASE_DIR:-}"
    if [[ -z "${base}" ]]; then
        if [[ -f "${root}/CMakeLists.txt" && -d "${root}/src/recomp" ]]; then base="${root}"; else base="${root}/.scratch/base"; fi
    fi
    if [[ ! -f "${base}/CMakeLists.txt" ]]; then
        local zip="${LMVR_ZIP:-}"
        [[ -n "${zip}" ]] || { echo "ERROR: no base tree: visionos/scripts/apply-base.sh <LMVR-Source-build150.zip> (or LMVR_ZIP=...)" >&2; exit 1; }
        "${visionos}/scripts/apply-base.sh" "${zip}" "${base}"
    fi
    # The development tree (.scratch/base) is made again when this repository's patches have changed
    # since apply-base.sh made it (it stamps their hash in the tree's .git): an old tree built
    # quietly without the newest base changes. Trees given by LMVR_BASE_DIR, and the release's own
    # source, are left alone.
    if [[ -z "${LMVR_BASE_DIR:-}" && "${base}" == "${root}/.scratch/base" ]]; then
        local want have zip
        want="$("${visionos}/scripts/apply-base.sh" --patch-hash)"
        have="$(cat "${base}/.git/lmvr-patches.stamp" 2>/dev/null || true)"
        if [[ "${have}" != "${want}" ]]; then
            # Never over work in it: uncommitted changes, or commits since apply-base.sh made it.
            if [[ -n "$(git -C "${base}" status --porcelain 2>/dev/null)" ||
                  "$(git -C "${base}" rev-parse HEAD 2>/dev/null)" != "$(cat "${base}/.git/lmvr-head" 2>/dev/null)" ]]; then
                echo "ERROR: visionos/patches/base changed since .scratch/base was made, and .scratch/base has work of its own (changes or commits): keep it (move it aside) and run visionos/scripts/apply-base.sh <LMVR-Source-build150.zip> again" >&2
                exit 1
            fi
            zip="${LMVR_ZIP:-$(cat "${base}/.git/lmvr-zip-path" 2>/dev/null || true)}"
            [[ -n "${zip}" && -f "${zip}" ]] || { echo "ERROR: visionos/patches/base changed since .scratch/base was made: run visionos/scripts/apply-base.sh <LMVR-Source-build150.zip> again (or set LMVR_ZIP)" >&2; exit 1; }
            echo "The base patches changed: making .scratch/base again" >&2
            "${visionos}/scripts/apply-base.sh" "${zip}" "${base}"
        fi
    fi

    # lmrecompile for this Mac (the base's own recompiler, built with Xcode's clang).
    local host="${root}/.scratch/host"
    local recompile="${host}/lmrecompile"
    mkdir -p "${host}"
    if [[ ! -x "${recompile}" || "${base}/src/recompiler/recompiler.cpp" -nt "${recompile}" || "${base}/tools/recomp/lmrecompile.cpp" -nt "${recompile}" ]]; then
        echo "Building lmrecompile for this Mac" >&2
        xcrun --sdk macosx clang++ -std=c++20 -O2 -I"${base}/src" "${base}/tools/recomp/lmrecompile.cpp" \
            "${base}/src/recompiler/recompiler.cpp" -o "${recompile}"
    fi

    # The player's main.dol, recompiled into C++ (32 chunks and a function table).
    local dol="${LMVR_DOL:-}" version="${LMVR_DOL_VERSION:-}" recomp_dir=""
    if [[ -z "${dol}" && -n "${LMVR_DISC:-}" ]]; then
        mkdir -p "${root}/.scratch/disc"
        dol="${root}/.scratch/disc/main.dol"
        version="$(python3 -I "${visionos}/scripts/extract-dol.py" "${LMVR_DISC}" "${dol}")"
    fi
    if [[ -n "${dol}" ]]; then
        [[ -f "${dol}" ]] || { echo "ERROR: ${dol} not found" >&2; exit 1; }
        version="${version:-GLME01}"
        recomp_dir="${root}/.scratch/recomp-${version}"
        local cfg=()
        if [[ "${version}" == "GLMP01" ]]; then
            cfg=(--symbols "${base}/decomp/config/GLMP01_00/symbols.txt" --version-map "${base}/tools/recomp/GLMP01_map.txt")
        else
            cfg=(--symbols "${base}/decomp/config/GLME01/symbols.txt" --splits "${base}/decomp/config/GLME01/splits.txt")
        fi
        local stamp_now
        stamp_now="$( { shasum -a 256 "${dol}" "${recompile}" "${base}/tools/recomp/hooks.txt" "${cfg[1]}"; echo "${version}"; } | shasum -a 256 | cut -c1-16)"
        if [[ ! -f "${recomp_dir}/func_table.cpp" || "$(cat "${recomp_dir}/.stamp" 2>/dev/null)" != "${stamp_now}" ]]; then
            echo "Recompiling ${dol} (${version})" >&2
            rm -rf "${recomp_dir}"
            mkdir -p "${recomp_dir}"
            "${recompile}" --dol "${dol}" --version "${version}" "${cfg[@]}" \
                --hooks "${base}/tools/recomp/hooks.txt" --out "${recomp_dir}" >&2
            echo "${stamp_now}" > "${recomp_dir}/.stamp"
        fi
    fi

    # Dawn for xrOS (cached by revision, patch and flags; ~1 h the first time).
    local dawn_package sysroot rust_target
    if [[ ${simulator} -eq 1 ]]; then
        dawn_package="$(LMVR_BASE_DIR="${base}" "${visionos}/scripts/build-dawn-visionos.sh" --simulator | tail -1)"
        sysroot=xrsimulator rust_target=aarch64-apple-visionos-sim
    else
        dawn_package="$(LMVR_BASE_DIR="${base}" "${visionos}/scripts/build-dawn-visionos.sh" | tail -1)"
        sysroot=xros rust_target=aarch64-apple-visionos
    fi
    # nod (aurora's disc reader) is Rust: a nightly with the visionOS targets, pinned by date (TPVR's).
    [[ -d /opt/homebrew/opt/rustup/bin ]] && export PATH="/opt/homebrew/opt/rustup/bin:${PATH}"
    local rust_toolchain="${LMVR_RUST_TOOLCHAIN:-nightly-2026-09-30}"
    rustup target list --toolchain "${rust_toolchain}" --installed 2>/dev/null | grep -qx "${rust_target}" || {
        echo "ERROR: Rust ${rust_toolchain} with ${rust_target} is needed:" >&2
        echo "  rustup toolchain install ${rust_toolchain} --profile minimal && rustup target add --toolchain ${rust_toolchain} ${rust_target}" >&2
        exit 1; }
    export XROS_DEPLOYMENT_TARGET=26.0
    # Host package prefixes CMake must not search: their libraries are macOS builds.
    local ignore="/opt/homebrew;/usr/local;/opt/homebrew/Caskroom/miniconda/base;${HOME}/miniconda3;${HOME}/miniforge3;${HOME}/anaconda3"
    [[ -n "${CONDA_PREFIX:-}" ]] && ignore="${ignore};${CONDA_PREFIX}"
    cmake -S "${base}" -B "${build}" -G Ninja \
        -DCMAKE_SYSTEM_NAME=visionOS \
        -DCMAKE_OSX_SYSROOT="${sysroot}" \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
        -DCMAKE_BUILD_TYPE="${config}" \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON \
        "-DCMAKE_IGNORE_PREFIX_PATH=${ignore}" \
        "-DLM_VISIONOS_CMAKE=${visionos}/cmake/framework.cmake" \
        "-DLMVR_RECOMP_DIR=${recomp_dir}" \
        -DLM_BUILD_VR_TESTS=OFF \
        -DCMAKE_MACOSX_BUNDLE=OFF \
        -DAURORA_DAWN_PROVIDER=package \
        "-DAURORA_DAWN_PACKAGE_URL=file://${dawn_package}" \
        -DAURORA_SDL3_PROVIDER=vendor \
        -DAURORA_NOD_PROVIDER=vendor \
        -DRust_CARGO_TARGET="${rust_target}" \
        "-DRust_TOOLCHAIN=${rust_toolchain}" \
        -DTRACY_ENABLE=OFF \
        -Ufmt_DIR -Uzstd_DIR
    cmake --build "${build}" --target LuigisMansionVisionGame --parallel "$(sysctl -n hw.ncpu)"
    # The shipped shader/pipeline precache list (not game data), for the app's resources.
    cp "${base}/data/pipelines.bin" "${build}/pipelines.bin"
}
if [[ ${stub} -eq 0 ]]; then
    build_game
    [[ -d "${build}/LuigisMansionVisionGame.framework" ]] || {
        echo "ERROR: ${build}/LuigisMansionVisionGame.framework was not produced" >&2; exit 1; }
    echo "Game framework: ${build}/LuigisMansionVisionGame.framework"
fi
[[ ${game_only} -eq 1 ]] && exit 0

# 2. The app. The launcher's kit is vendored, never fetched at build time.
[[ -f "${visionos}/TrevorbiltKit/Package.swift" ]] || {
    echo "ERROR: visionos/TrevorbiltKit is missing: run visionos/scripts/sync-trevorbilt-kit.sh" >&2; exit 1; }
# Clean-built every time: Xcode's incremental build (especially in a
# shared custom build location) has kept stale embedded frameworks and stale
# Swift objects. The app is a handful of files; the clean costs seconds.
# A clean of one platform's products can delete the other's in a shared build
# location, so rebuild the Simulator app before Simulator tests after a device build.
#
# Where xcodebuild put the product: Xcode's "Custom" build-location preference
# (IDEBuildLocationStyle) overrides -derivedDataPath, so ask xcodebuild.
products_dir() {
    xcodebuild "$@" -showBuildSettings 2>/dev/null | awk -F' = ' '/^ *BUILT_PRODUCTS_DIR = / {print $2; exit}'
}
app_dir="${visionos}/App"
# Our own products and intermediates (WW's lesson): this Mac's Xcode puts every project's products in
# one shared "Custom" location, where another port's clean build deleted the app. SYMROOT/OBJROOT on
# the command line override that preference.
xcode_dirs=(SYMROOT="${root}/.scratch/xcode/Products" OBJROOT="${root}/.scratch/xcode/Intermediates")
if [[ ${simulator} -eq 1 ]]; then
    export LMVR_TEAM="${team:-NONE}" LMVR_BUNDLE_ID="${bundle_id:-dev.lmvr.vision.simulator}" LMVR_GAME_BUILD_DIR="${build}"
    (cd "${app_dir}" && xcodegen generate --quiet)
    args=(-project "${app_dir}/LuigisMansionVision.xcodeproj" -scheme LuigisMansionVision -configuration "${xcode_config}"
          -destination 'generic/platform=visionOS Simulator' -derivedDataPath "${root}/.scratch/DerivedData-simulator"
          ARCHS=arm64 CODE_SIGNING_ALLOWED=NO "${xcode_dirs[@]}")
    xcodebuild "${args[@]}" clean build
    echo "App: $(products_dir "${args[@]}")/LuigisMansionVision.app"
    exit 0
fi
[[ -n "${team}" ]] || { echo "ERROR: --team TEAMID is needed to sign the app" >&2; exit 2; }
[[ -n "${bundle_id}" ]] || bundle_id="dev.lmvr.vision.${team}"
export LMVR_TEAM="${team}" LMVR_BUNDLE_ID="${bundle_id}" LMVR_GAME_BUILD_DIR="${build}"
(cd "${app_dir}" && xcodegen generate --quiet)
args=(-project "${app_dir}/LuigisMansionVision.xcodeproj" -scheme LuigisMansionVision -configuration "${xcode_config}"
      -destination 'generic/platform=visionOS' -derivedDataPath "${root}/.scratch/DerivedData"
      DEVELOPMENT_TEAM="${team}" PRODUCT_BUNDLE_IDENTIFIER="${bundle_id}" "${xcode_dirs[@]}")
xcodebuild "${args[@]}" -allowProvisioningUpdates clean build
app="$(products_dir "${args[@]}")/LuigisMansionVision.app"
echo "App: ${app}"

# 3. Install. A locked or asleep headset makes devicectl hang: wake and unlock it first.
if [[ ${install} -eq 1 ]]; then
    if [[ -z "${device}" ]]; then
        json="$(mktemp)"
        xcrun devicectl list devices --json-output "${json}" >/dev/null 2>&1 || true
        device="$(python3 - "${json}" <<'PY'
import json, sys
try:
    devices = json.load(open(sys.argv[1]))["result"]["devices"]
except Exception:
    devices = []
for d in devices:
    hw = d.get("hardwareProperties", {})
    if hw.get("platform") == "visionOS" and hw.get("reality") == "physical":
        print(hw.get("udid", d.get("identifier", "")))
        break
PY
)"
        rm -f "${json}"
    fi
    [[ -n "${device}" ]] || { echo "ERROR: no paired Apple Vision Pro found; pass --device UDID" >&2; exit 1; }
    xcrun devicectl device install app --device "${device}" "${app}"
fi
