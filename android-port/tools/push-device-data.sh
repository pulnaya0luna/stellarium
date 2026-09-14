#!/usr/bin/env bash
# Deploy Stellarium's runtime data to a connected Android device/emulator.
#
# Stellarium cannot find its install directory on Android without this, and the
# failure modes are ugly (see android-port/README.md). Upstream's Android search
# path in StelFileMgr::init() covers /sdcard/stellarium, so that is where the
# data goes.
#
# Usage:  android-port/tools/push-device-data.sh [serial]
#         (serial optional; omit if exactly one device is attached)

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SDK="${ANDROID_SDK_ROOT:-C:/Users/Raka/AppData/Local/Android/Sdk}"
ADB="$SDK/platform-tools/adb.exe"
PKG="org.stellarium.stellarium"
DEST="/sdcard/stellarium"

if [ $# -ge 1 ]; then ADB_ARGS=(-s "$1"); else ADB_ARGS=(); fi

echo "== device =="
"$ADB" "${ADB_ARGS[@]}" devices | tail -n +2

# 1. Permissions: without MANAGE_EXTERNAL_STORAGE the app is denied read access to
#    /sdcard/stellarium and dies in copyDefaultConfigFile(). Note --uid: a plain
#    `appops set` writes the wrong (non-uid) mode and does not take effect.
echo "== permissions =="
"$ADB" "${ADB_ARGS[@]}" shell appops set --uid "$PKG" MANAGE_EXTERNAL_STORAGE allow
"$ADB" "${ADB_ARGS[@]}" shell appops get --uid "$PKG" MANAGE_EXTERNAL_STORAGE

# 2. The three data sets. All three are required:
#    - data/       contains ssystem_major.ini, the file StelFileMgr validates
#    - landscapes/ contains 'zero', whose absence recurses to a stack overflow
#    - textures/   contains saturn_rings_radial.png, whose absence SIGSEGVs
#                  Planet::drawSphere via an unguarded rings->tex->bind()
echo "== data/ =="
"$ADB" "${ADB_ARGS[@]}" shell mkdir -p "$DEST/data"
"$ADB" "${ADB_ARGS[@]}" push "$REPO/data/." "$DEST/data/"

echo "== landscapes/ =="
"$ADB" "${ADB_ARGS[@]}" shell mkdir -p "$DEST/landscapes"
"$ADB" "${ADB_ARGS[@]}" push "$REPO/landscapes/." "$DEST/landscapes/"

echo "== textures/ =="
"$ADB" "${ADB_ARGS[@]}" shell mkdir -p "$DEST/textures"
"$ADB" "${ADB_ARGS[@]}" push "$REPO/textures/." "$DEST/textures/"

# 3. Verify the specific files whose absence produces the confusing crashes above.
echo "== verify =="
for f in "data/ssystem_major.ini" "landscapes/zero/landscape.ini" "textures/saturn_rings_radial.png"; do
	if "$ADB" "${ADB_ARGS[@]}" shell "test -f $DEST/$f" 2>/dev/null; then
		echo "  OK      $f"
	else
		echo "  MISSING $f"
		exit 1
	fi
done

echo
echo "Done. Launch with:"
echo "  $ADB shell am start -S -W -n $PKG/org.qtproject.qt.android.bindings.QtActivity"
