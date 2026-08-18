#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

output=${1:-dist/erosqnative/nx240-stress.nxv}

ffmpeg -hide_banner -y \
    -f lavfi -i "testsrc2=size=320x240:rate=24:duration=20" \
    -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=20" \
    -map 0:v:0 -map 1:a:0 \
    -vf "noise=alls=18:allf=t+u" \
    -c:v libx264 -preset slow -profile:v baseline -level:v 1.3 \
    -pix_fmt yuv420p -refs 1 -bf 0 -g 48 -keyint_min 48 -sc_threshold 0 \
    -b:v 680k -maxrate 768k -bufsize 1000k \
    -c:a aac -profile:a aac_low -ar 44100 -ac 2 -b:a 80k \
    -movflags +faststart -f mp4 "$output"
