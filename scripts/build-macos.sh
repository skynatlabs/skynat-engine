#!/bin/bash
# build-macos.sh <arm64|x86_64> <deployment-target>
#
# Full build of skynat-engine for one Mac architecture:
#   1. clone OBS Studio $OBS_TAG (unmodified) into $WORK/obs-studio, once
#   2. configure OBS headless with its own "macos" preset (Xcode generator);
#      OBS's CMake downloads its prebuilt obs-deps (universal arm64+x86_64)
#   3. build libobs, the OpenGL renderer and only the plugins the engine loads
#   4. build skynat-engine against that libobs
#   5. package dist/darwin-<arch>/ (scripts/package-macos.sh)
#
# Examples:
#   scripts/build-macos.sh arm64 11.0      # Apple Silicon
#   scripts/build-macos.sh x86_64 11.0     # Intel / older Macs — cross-compiles on Apple Silicon
#
# Environment:
#   SKYNAT_ENGINE_WORK  work folder for the OBS checkout and build trees
#                       (default: ../skynat-engine-work next to this repo)
#   OBS_TAG             OBS Studio release tag (default 31.0.4 — see NOTICE.md)
#   JOBS                parallel compile jobs (default 4)
#   SKYNAT_HEAVY_LOCK   optional lock directory: configure/build steps wait for it
#                       (mkdir lock, stale after 90 minutes) so heavy jobs never overlap
#   CLEAN=1             delete this architecture's build trees first
set -euo pipefail

ARCH="${1:-}"
TARGET="${2:-}"
if [[ "$ARCH" != "arm64" && "$ARCH" != "x86_64" ]] || [[ -z "$TARGET" ]]; then
  echo "usage: $0 <arm64|x86_64> <deployment-target>   e.g. $0 arm64 11.0" >&2
  exit 64
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${SKYNAT_ENGINE_WORK:-$(cd "$ROOT/.." && pwd)/skynat-engine-work}"
OBS_TAG="${OBS_TAG:-31.0.4}"
JOBS="${JOBS:-4}"
OBS_SRC="$WORK/obs-studio"
OBS_BUILD="$OBS_SRC/build_macos_$ARCH"
ENGINE_BUILD="$WORK/engine-build-$ARCH"
CONFIG=Release

OBS_TARGETS=(libobs libobs-opengl mac-avcapture mac-videotoolbox coreaudio-encoder obs-ffmpeg image-source obs-transitions obs-filters obs-outputs obs-x264)

log() { printf '\n== %s\n' "$*"; }
die() { echo "build-macos: $*" >&2; exit 1; }

have_lock=0
lock() {
  [[ -n "${SKYNAT_HEAVY_LOCK:-}" ]] || return 0
  until mkdir "$SKYNAT_HEAVY_LOCK" 2>/dev/null; do
    if [[ -d "$SKYNAT_HEAVY_LOCK" ]] && (( $(date +%s) - $(stat -f %m "$SKYNAT_HEAVY_LOCK") > 5400 )); then
      rmdir "$SKYNAT_HEAVY_LOCK" 2>/dev/null || true
    fi
    sleep 20
  done
  have_lock=1
}
unlock() {
  if [[ $have_lock == 1 ]]; then rmdir "$SKYNAT_HEAVY_LOCK" 2>/dev/null || true; have_lock=0; fi
}
trap unlock EXIT INT TERM

# ---- tools -------------------------------------------------------------------
command -v cmake >/dev/null || die "cmake not found (brew install cmake)"
command -v xcodebuild >/dev/null || die "Xcode not found"
CMAKE_VERSION="$(cmake --version | awk 'NR==1{print $3}')"
[[ "$(printf '%s\n3.28\n' "$CMAKE_VERSION" | sort -V | head -1)" == "3.28" ]] || die "CMake 3.28+ needed (have $CMAKE_VERSION)"
log "skynat-engine for $ARCH, macOS $TARGET+, OBS $OBS_TAG, work folder $WORK"
mkdir -p "$WORK"

# ---- 1. OBS source ---------------------------------------------------------
if [[ ! -d "$OBS_SRC/.git" ]]; then
  log "cloning OBS Studio $OBS_TAG"
  git clone --depth 1 --branch "$OBS_TAG" --recurse-submodules --shallow-submodules \
    https://github.com/obsproject/obs-studio "$OBS_SRC"
fi
HAVE_TAG="$(git -C "$OBS_SRC" describe --tags --exact-match 2>/dev/null || true)"
[[ "$HAVE_TAG" == "$OBS_TAG" ]] || die "$OBS_SRC is at '$HAVE_TAG', expected $OBS_TAG (remove it to re-clone)"
if [[ -n "$(git -C "$OBS_SRC" status --porcelain --untracked-files=no)" ]]; then
  die "$OBS_SRC has local changes — the engine is built from unmodified OBS"
fi
if [[ -d "$ROOT/patches" ]] && ls "$ROOT"/patches/*.patch >/dev/null 2>&1; then
  die "patches/ is not empty; apply them deliberately and record them in NOTICE.md first"
fi

if [[ "${CLEAN:-0}" == "1" ]]; then
  rm -rf "$OBS_BUILD" "$ENGINE_BUILD"
fi

# ---- 2. configure OBS (headless) ---------------------------------------------
log "configuring OBS (Xcode generator, headless)"
lock
cd "$OBS_SRC"   # presets are read from the source folder
nice -n 15 cmake --preset macos -B "$OBS_BUILD" \
  -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$TARGET" \
  -DENABLE_UI=OFF -DENABLE_SCRIPTING=OFF -DENABLE_BROWSER=OFF -DENABLE_WEBSOCKET=OFF \
  -DENABLE_VST=OFF -DENABLE_AJA=OFF -DENABLE_DECKLINK=OFF -DENABLE_VIRTUALCAM=OFF \
  -DENABLE_SYPHON=OFF -DENABLE_VLC=OFF -DENABLE_FREETYPE=OFF -DENABLE_WEBRTC=OFF \
  -DENABLE_NEW_MPEGTS_OUTPUT=OFF -DENABLE_SERVICE_UPDATES=OFF -DENABLE_CCACHE=OFF \
  -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF -DOBS_CODESIGN_IDENTITY=-

cd "$ROOT"

# ---- 3. build libobs + renderer + plugins -------------------------------------
log "building ${OBS_TARGETS[*]}"
nice -n 15 cmake --build "$OBS_BUILD" --config "$CONFIG" --parallel "$JOBS" --target "${OBS_TARGETS[@]}" -- -quiet

# ---- 4. build the engine -------------------------------------------------------
log "building skynat-engine"
nice -n 15 cmake -S "$ROOT" -B "$ENGINE_BUILD" \
  -DCMAKE_BUILD_TYPE="$CONFIG" \
  -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$TARGET" \
  -Dlibobs_DIR="$OBS_BUILD/libobs"
nice -n 15 cmake --build "$ENGINE_BUILD" --parallel "$JOBS"
unlock

# ---- 5. package ----------------------------------------------------------------
log "packaging"
OBS_BUILD_DIR="$OBS_BUILD" ENGINE_BUILD_DIR="$ENGINE_BUILD" OBS_CONFIG="$CONFIG" SKYNAT_ENGINE_WORK="$WORK" \
  "$ROOT/scripts/package-macos.sh" "$ARCH" "$TARGET"

log "done: $ROOT/dist/darwin-$ARCH"
