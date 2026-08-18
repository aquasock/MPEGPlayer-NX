#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /path/to/rockbox" >&2
    exit 2
fi

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
rockbox_dir=$(CDPATH= cd -- "$1" && pwd)
plugins_dir="$rockbox_dir/apps/plugins"
target_dir="$plugins_dir/mpegplayer_nx"

if [ ! -f "$plugins_dir/SUBDIRS" ] || [ ! -f "$plugins_dir/viewers.config" ]; then
    echo "error: $rockbox_dir does not look like a Rockbox source tree" >&2
    exit 1
fi

mkdir -p "$target_dir"
cp -R "$project_dir"/plugin/. "$target_dir"/

if ! grep -qx 'mpegplayer_nx' "$plugins_dir/SUBDIRS"; then
    printf '\nmpegplayer_nx\n' >> "$plugins_dir/SUBDIRS"
fi

if ! grep -qx 'mpegplayer_nx,viewers' "$plugins_dir/CATEGORIES"; then
    printf '\nmpegplayer_nx,viewers\n' >> "$plugins_dir/CATEGORIES"
fi

if ! grep -qx 'nxv,viewers/mpegplayer_nx,4' "$plugins_dir/viewers.config"; then
    printf '\nnxv,viewers/mpegplayer_nx,4\n' >> "$plugins_dir/viewers.config"
fi

echo "MPEGPlayer NX staged in $target_dir"
echo "Build from your configured Rockbox build directory."
