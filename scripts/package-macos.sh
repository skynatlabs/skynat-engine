#!/bin/bash
# package-macos.sh <arm64|x86_64> <deployment-target>
#
# Collects the engine, libobs, the renderer, the plugins it loads and the
# FFmpeg/mbedTLS/x264 libraries they link into dist/darwin-<arch>/:
#
#   dist/darwin-<arch>/
#     bin/skynat-engine            the program (LC_RPATH @executable_path/../Frameworks)
#     Frameworks/libobs.framework  libobs + its data (effects) in Resources/
#     Frameworks/libobs-opengl.dylib
#     Frameworks/lib*.dylib        third-party libraries from obs-deps (thinned)
#     PlugIns/<module>.plugin      the OBS modules the engine loads
#     COPYING  NOTICE.md  README.md  SOURCE.txt  licenses/
#
# libobs and its plugins already look for libraries in
# @executable_path/../Frameworks (the layout of an app bundle), so no binary
# is patched: the folder works wherever it is copied, e.g. as
# <App>.app/Contents/Resources/engine/. Everything is ad-hoc signed for
# local runs; electron-builder re-signs it with the real identity later.
#
# Environment: SKYNAT_ENGINE_WORK (default ../skynat-engine-work),
#              OBS_BUILD_DIR, ENGINE_BUILD_DIR, OBS_CONFIG (Release), SKIP_SIGN=1
set -euo pipefail

ARCH="${1:-}"
TARGET="${2:-}"
if [[ "$ARCH" != "arm64" && "$ARCH" != "x86_64" ]] || [[ -z "$TARGET" ]]; then
  echo "usage: $0 <arm64|x86_64> <deployment-target>" >&2
  exit 64
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${SKYNAT_ENGINE_WORK:-$(cd "$ROOT/.." && pwd)/skynat-engine-work}"
OBS_SRC="$WORK/obs-studio"
OBS_BUILD="${OBS_BUILD_DIR:-$OBS_SRC/build_macos_$ARCH}"
ENGINE_BUILD="${ENGINE_BUILD_DIR:-$WORK/engine-build-$ARCH}"
CONFIG="${OBS_CONFIG:-Release}"
DEPS_DIR="$(ls -d "$OBS_SRC"/.deps/obs-deps-*-universal 2>/dev/null | head -1 || true)"
OUT="$ROOT/dist/darwin-$ARCH"

PLUGINS=(mac-avcapture mac-videotoolbox coreaudio-encoder obs-ffmpeg image-source obs-transitions obs-filters obs-outputs obs-x264)

die() { echo "package-macos: $*" >&2; exit 1; }
[[ -x "$ENGINE_BUILD/skynat-engine" ]] || die "engine not built: $ENGINE_BUILD/skynat-engine"
[[ -d "$OBS_BUILD/libobs/$CONFIG/libobs.framework" ]] || die "libobs not built: $OBS_BUILD/libobs/$CONFIG"
[[ -n "$DEPS_DIR" && -d "$DEPS_DIR/lib" ]] || die "obs-deps not found under $OBS_SRC/.deps"

echo "== packaging skynat-engine for $ARCH (macOS $TARGET+) into $OUT"
rm -rf "$OUT"
mkdir -p "$OUT/bin" "$OUT/Frameworks" "$OUT/PlugIns" "$OUT/licenses"

cp "$ENGINE_BUILD/skynat-engine" "$OUT/bin/skynat-engine"
ditto "$OBS_BUILD/libobs/$CONFIG/libobs.framework" "$OUT/Frameworks/libobs.framework"
# Headers are for building against libobs, not for running it.
rm -rf "$OUT/Frameworks/libobs.framework/Headers" "$OUT/Frameworks/libobs.framework/Versions/A/Headers"
cp "$OBS_BUILD/libobs-opengl/$CONFIG/libobs-opengl.dylib" "$OUT/Frameworks/"
for p in "${PLUGINS[@]}"; do
  [[ -d "$OBS_BUILD/plugins/$p/$CONFIG/$p.plugin" ]] || die "plugin not built: $p"
  ditto "$OBS_BUILD/plugins/$p/$CONFIG/$p.plugin" "$OUT/PlugIns/$p.plugin"
done

# Every Mach-O file in the package (follows nothing outside $OUT).
macho_files() {
  find "$OUT" -type f \( -perm -u+x -o -name '*.dylib' \) -print0 | while IFS= read -r -d '' f; do
    if file -b "$f" | grep -q 'Mach-O'; then printf '%s\n' "$f"; fi
  done
}

# Pull in every @rpath library the package references, until nothing new appears.
while :; do
  added=0
  while IFS= read -r f; do
    while IFS= read -r dep; do
      name="${dep#@rpath/}"
      case "$name" in
        libobs.framework/*|libobs-opengl.dylib) continue ;;
      esac
      if [[ ! -e "$OUT/Frameworks/$name" ]]; then
        [[ -e "$DEPS_DIR/lib/$name" ]] || die "missing dependency $name (needed by ${f#$OUT/})"
        cp -L "$DEPS_DIR/lib/$name" "$OUT/Frameworks/$name"
        chmod 755 "$OUT/Frameworks/$name"
        added=1
      fi
    done < <(otool -L "$f" | awk 'NR>1 {print $1}' | grep '^@rpath/' || true)
  done < <(macho_files)
  [[ $added == 0 ]] && break
done

# Thin universal libraries down to the one architecture.
while IFS= read -r f; do
  archs="$(lipo -archs "$f" 2>/dev/null || true)"
  if [[ "$archs" == *" "* ]]; then
    lipo -thin "$ARCH" "$f" -output "$f.thin" && mv "$f.thin" "$f"
  fi
  [[ "$(lipo -archs "$f")" == "$ARCH" ]] || die "${f#$OUT/} is $(lipo -archs "$f"), not $ARCH"
done < <(macho_files)

# Every binary must run on the deployment target.
too_new=0
while IFS= read -r f; do
  minos="$(otool -l "$f" | awk '/LC_BUILD_VERSION/{b=1} b&&/minos/{print $2; exit} /LC_VERSION_MIN_MACOSX/{v=1} v&&/version/{print $2; exit}')"
  if [[ -n "$minos" ]] && [[ "$(printf '%s\n%s\n' "$minos" "$TARGET" | sort -V | tail -1)" != "$TARGET" ]]; then
    echo "  ! ${f#$OUT/} needs macOS $minos (> $TARGET)" >&2
    too_new=1
  fi
done < <(macho_files)
[[ $too_new == 0 ]] || die "some binaries need a newer macOS than $TARGET"

# Smaller files: drop local symbols (debug info stays in the build tree's dSYMs).
while IFS= read -r f; do
  strip -x "$f" 2>/dev/null || true
done < <(macho_files)

# Licences and the written source offer that travel with the binaries.
cp "$ROOT/COPYING" "$ROOT/NOTICE.md" "$ROOT/README.md" "$OUT/"
cp -R "$DEPS_DIR/licenses/." "$OUT/licenses/"
cp "$OBS_SRC/COPYING" "$OUT/licenses/OBS-Studio-COPYING"
OBS_TAG="$(git -C "$OBS_SRC" describe --tags 2>/dev/null || echo unknown)"
ENGINE_REV="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
cat > "$OUT/SOURCE.txt" <<EOF
skynat-engine is free software under the GNU General Public License,
version 2 or (at your option) any later version. See COPYING.

It is built from OBS Studio's libobs (https://github.com/obsproject/obs-studio,
tag $OBS_TAG, unmodified) and the OBS project's prebuilt dependencies
(https://github.com/obsproject/obs-deps). Licences of the third-party
libraries are in licenses/.

Complete corresponding source code: https://github.com/skynatlabs/skynat-engine
(engine revision $ENGINE_REV). On request, Skynat will also provide the source
on a physical medium for three years from the date you received this program.
EOF

if [[ "${SKIP_SIGN:-0}" != "1" ]]; then
  # Inside-out ad-hoc signature: libraries, framework, plugins, program.
  for f in "$OUT"/Frameworks/*.dylib; do codesign --force --sign - "$f" >/dev/null; done
  codesign --force --sign - "$OUT/Frameworks/libobs.framework" >/dev/null
  for p in "$OUT"/PlugIns/*.plugin; do codesign --force --sign - "$p" >/dev/null; done
  codesign --force --sign - "$OUT/bin/skynat-engine" >/dev/null
  codesign --verify --strict "$OUT/Frameworks/libobs.framework"
  for p in "$OUT"/PlugIns/*.plugin; do codesign --verify --strict "$p"; done
  codesign --verify --strict "$OUT/bin/skynat-engine"
fi

echo "== done: $(du -sh "$OUT" | cut -f1) in $OUT"
"$OUT/bin/skynat-engine" --version
