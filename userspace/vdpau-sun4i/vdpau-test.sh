#!/bin/sh
# SPDX-License-Identifier: MIT
# Target side: decode clips with stock `ffmpeg -hwaccel vdpau` through
# libvdpau_sun4i-drm and compare every frame (framemd5) with FFmpeg's
# software decoder. Needs an X display (VDPAU device creation).
# Usage: vdpau-test.sh <clip...>   → one "RESULT <clip> PASS|FAIL ..." line each
set -u
export DISPLAY=${DISPLAY:-:0} XAUTHORITY=${XAUTHORITY:-$HOME/.Xauthority}
export VDPAU_DRIVER=sun4i-drm
HERE=$(cd "$(dirname "$0")" && pwd)
export VDPAU_DRIVER_PATH=${VDPAU_DRIVER_PATH:-$HERE}
[ -e "$HERE/libvdpau_sun4i-drm.so" ] || ln -sf libvdpau_sun4i-drm.so.1 "$HERE/libvdpau_sun4i-drm.so"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
rc=0
for clip in "$@"; do
	n=$(basename "$clip")
	ffmpeg -y -v error -i "$clip" -an -f framemd5 -pix_fmt nv12 "$T/sw.md5" 2>"$T/sw.err"
	ffmpeg -y -v error -hwaccel vdpau -hwaccel_output_format vdpau -i "$clip" -an \
		-vf hwdownload,format=nv12 -f framemd5 "$T/hw.md5" 2>"$T/hw.err"
	frames=$(grep -vc '^#' "$T/sw.md5")
	hwframes=$(grep -vc '^#' "$T/hw.md5")
	diff=$(paste "$T/sw.md5" "$T/hw.md5" | grep -v '^#' | awk -F'\t' '{split($1,a,","); split($2,b,","); if (a[6] != b[6]) n++} END {print n+0}')
	first=$(paste "$T/sw.md5" "$T/hw.md5" | grep -v '^#' | awk -F'\t' '{split($1,a,","); split($2,b,","); if (a[6] != b[6]) {print NR-1; exit}}')
	if [ "$frames" -gt 0 ] && [ "$frames" = "$hwframes" ] && [ "$diff" = 0 ]; then
		echo "RESULT $n PASS frames=$frames"
	else
		echo "RESULT $n FAIL frames=$frames hwframes=$hwframes differing=$diff first_diff=${first:-none}"
		sed 's/^/  hw: /' "$T/hw.err" | head -5
		rc=1
	fi
done
exit $rc
