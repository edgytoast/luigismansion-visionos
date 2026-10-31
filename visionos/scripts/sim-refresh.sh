#!/usr/bin/env bash
# Simulator iteration on the game alone: rebuilds the framework (--game-only), copies it into the
# last Simulator app build (unsigned, so a swapped framework still runs) and reinstalls the app on
# the given Simulator. Minutes instead of a clean app build. Pass the same LMVR_* environment as
# the full build (LMVR_DISC, LMVR_BASE_DIR).
#   visionos/scripts/sim-refresh.sh <simulator udid>
set -euo pipefail
root="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
sim="${1:?usage: sim-refresh.sh <simulator udid>}"
"${root}/visionos/scripts/build-visionos.sh" --simulator --game-only
app="${root}/.scratch/xcode/Products/Release-xrsimulator/LuigisMansionVision.app"
framework="${root}/.scratch/build-xrsimulator-RelWithDebInfo/LuigisMansionVisionGame.framework"
[[ -d "${app}" ]] || { echo "ERROR: no Simulator app yet: build-visionos.sh --simulator first" >&2; exit 1; }
cp "${framework}/LuigisMansionVisionGame" "${app}/Frameworks/LuigisMansionVisionGame.framework/LuigisMansionVisionGame"
xcrun simctl install "${sim}" "${app}"
echo "Reinstalled ${app} on ${sim}"
