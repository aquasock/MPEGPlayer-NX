#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_file="$project_dir/dist/erosqnative/nx240-av-sync.nxv"
output=${1:-"$project_dir/dist/erosqnative/nx240-endurance.nxv"}
duration=${2:-300}

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "error: ffmpeg is required" >&2
    exit 1
fi
if [ ! -f "$source_file" ]; then
    echo "error: create nx240-av-sync.nxv first" >&2
    exit 1
fi

mkdir -p "$(dirname -- "$output")"

# Repeat the hardware-validated sync clip without re-encoding. This creates a
# longer sample table and duration while preserving the exact NX240 streams.
ffmpeg -hide_banner -loglevel warning -y \
    -stream_loop -1 -i "$source_file" -t "$duration" \
    -map 0:v:0 -map 0:a:0 -c copy -movflags +faststart -f mp4 "$output"

echo "wrote $output ($duration seconds)"
