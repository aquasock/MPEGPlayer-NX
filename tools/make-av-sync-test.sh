#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=${1:-"$project_dir/dist/erosqnative/nx240-av-sync.nxv"}
font=/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "error: ffmpeg is required" >&2
    exit 1
fi
if [ ! -f "$font" ]; then
    echo "error: expected test font at $font" >&2
    exit 1
fi

mkdir -p "$(dirname -- "$output")"

# The center square flashes white for exactly two 24 fps frames at every
# integer second. A stereo 1 kHz tone begins at the same instants and lasts
# 80 ms, making both fixed offset and long-term drift easy to perceive.
ffmpeg -hide_banner -loglevel warning -y \
    -f lavfi -i "testsrc2=size=320x240:rate=24:duration=60" \
    -f lavfi -i "aevalsrc=0.22*sin(2*PI*1000*t)*lt(mod(t\,1)\,0.08):s=44100:d=60" \
    -map 0:v:0 -map 1:a:0 \
    -vf "drawbox=x=112:y=66:w=96:h=96:color=black@0.75:t=fill,drawbox=x=112:y=66:w=96:h=96:color=white:t=fill:enable='lt(mod(t,1),0.083334)',drawtext=fontfile=$font:text='FLASH = BEEP':fontcolor=white:fontsize=22:borderw=2:bordercolor=black:x=(w-text_w)/2:y=12,drawtext=fontfile=$font:text='%{pts\\:hms}':fontcolor=white:fontsize=24:borderw=2:bordercolor=black:x=(w-text_w)/2:y=195" \
    -c:v libx264 -profile:v baseline -level:v 1.3 \
    -pix_fmt yuv420p -refs 1 -bf 0 -g 48 \
    -b:v 500k -maxrate 700k -bufsize 1000k \
    -c:a aac -profile:a aac_low -ar 44100 -ac 2 -b:a 80k \
    -movflags +faststart -f mp4 "$output"

echo "wrote $output"
