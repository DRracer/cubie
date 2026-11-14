#!/bin/sh
# SPDX-License-Identifier: MIT
# Target side: per-frame comparison of `ffmpeg -hwaccel vdpau` (libvdpau_sun4i-drm)
# against FFmpeg's software decoder for one clip.
# Prints: output fingerprint (md5 over all frame hashes), Y-PSNR average/min,
# and a display-order frame-type string with '.' = bit-exact, the type letter
# (I/P/B) = differs.
# Usage: vdpau-frames.sh <clip>
set -u
export DISPLAY=${DISPLAY:-:0} XAUTHORITY=${XAUTHORITY:-$HOME/.Xauthority}
export VDPAU_DRIVER=sun4i-drm
HERE=$(cd "$(dirname "$0")" && pwd)
export VDPAU_DRIVER_PATH=${VDPAU_DRIVER_PATH:-$HERE}
clip=$1
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
ffmpeg -v error -i "$clip" -an -pix_fmt nv12 -f framemd5 "$T/sw.md5"
ffmpeg -v error -hwaccel vdpau -hwaccel_output_format vdpau -i "$clip" -an \
	-vf hwdownload,format=nv12 -f framemd5 "$T/hw.md5"
ffmpeg -v info -hwaccel vdpau -hwaccel_output_format vdpau -i "$clip" -i "$clip" -an \
	-lavfi "[0:v]hwdownload,format=nv12[a];[1:v]format=nv12[b];[a][b]psnr=stats_file=$T/psnr.txt" \
	-f null - 2>&1 | grep -o 'PSNR y:[0-9.inf]*' | head -1
grep -v '^#' "$T/hw.md5" | awk -F', *' '{print $6}' > "$T/hw.h"
grep -v '^#' "$T/sw.md5" | awk -F', *' '{print $6}' > "$T/sw.h"
echo "fingerprint $(md5sum < "$T/hw.h" | cut -c1-12)  exact $(paste -d' ' "$T/sw.h" "$T/hw.h" | awk '$1==$2' | wc -l)/$(wc -l < "$T/sw.h")"
awk '{split($6,a,":"); v=a[2]; if (v!="inf" && (m=="" || v+0<m)) m=v+0} END {print "min Y-PSNR", m}' "$T/psnr.txt"
ffprobe -v error -select_streams v:0 -show_entries frame=pict_type -of csv=p=0 "$clip" | tr -d ',' > "$T/types"
paste -d' ' "$T/sw.h" "$T/hw.h" "$T/types" | awk '{printf "%s", ($1==$2) ? "." : $3} NR%60==0 {print ""} END {print ""}'
