# MPEGPlayer NX

MPEGPlayer NX is a new, independent video player for Rockbox. Its first target
is the HIFI WALKER H2 / AIGO Eros Q native port (320x240, RGB565).

This is **not** a fork or replacement for Rockbox MPEGPlayer. The legacy
MPEG-1/MPEG-2 plugin remains untouched. NX starts with an intentionally narrow
modern media profile and a clean player architecture.

## Target media profile (NX240)

- ISO Base Media File Format with the Rockbox-facing `.nxv` extension
- H.264/AVC Constrained Baseline, Level 1.3
- 320x240 maximum, progressive, 8-bit YUV 4:2:0
- CAVLC, no B-frames, one reference frame
- 30 fps maximum
- AAC-LC mono or stereo, 44.1/48 kHz
- `moov` before `mdat` (FFmpeg `-movflags +faststart`)

These limits are a performance contract, not an expression of everything the
container can store. Unsupported profiles should fail clearly.

## What works

- A portable, allocation-free top-level MP4 box probe
- Detection of `ftyp`, `moov`, `mdat`, 32-bit and extended-size boxes
- Detection of fast-start layout
- Parsing of `avc1`/`avcC` and `mp4a` track metadata
- Bounded parsing of `stts`, `stsc`, `stsz`, `stco`/`co64`, and `stss`
- Allocation-free iteration over timestamped compressed samples
- Extraction of AVC decoder configuration and length-prefixed NAL units
- Apache-licensed `h264bsd` Baseline decoding from a bounded TLSF pool
- Continuous 24 fps playback through Rockbox's native YUV420 blitter
- AAC-LC decode through Rockbox's fixed-point FAAD library
- 128 KiB PCM ring output with audio-master A/V synchronization
- Pause/resume, late-video dropping, underrun tracking, and exit controls
- Backlight held on during playback, with Rockbox's setting restored on exit
- Two-second elapsed/total-time overlay on startup or Menu, with a progress bar
- Live volume control using the player's dedicated volume buttons
- End-of-playback average/worst decode time, peak memory, and frame statistics
- Clean PCM drain at end-of-file without truncating buffered AAC
- Bounded silent-tail scheduling for final H.264 pictures after AAC padding
- Early, specific NX240 profile errors for size, codec, frame rate, and audio
- Sub-millisecond decoder timing on the X1000 hardware counter
- Host-side parser and 192-frame H.264 stream tests; the reference frame
  matches FFmpeg exactly
- A Rockbox subdirectory-plugin make fragment
- Idempotent staging into a Rockbox source tree

The hardware-validated M6 build plays H.264/AAC files with audio-master
synchronization, player controls, keyframe seeking, robust end-of-file draining,
specific media-profile diagnostics, and a clean five-minute endurance pass. See
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Measured results from the physical
HIFI WALKER H2 are recorded in
[docs/HARDWARE_VALIDATION.md](docs/HARDWARE_VALIDATION.md).

## Host build

```sh
make test
```

The host build exercises the same MP4 parser and complete H.264 stream path used
by the Rockbox plugin.

## Device stress benchmark

`dist/erosqnative/nx240-stress.nxv` is a 20-second, 480-frame synthetic motion
and fine-detail stress clip. It is deliberately harder to decode than the
color-bar correctness test. Recreate it with:

```sh
./tools/make-stress-video.sh
```

The clip's AAC track is a clearly audible 440 Hz test tone. A successful M4 run
should play the tone continuously and finish with zero (or very few) underruns.

## A/V synchronization test

`dist/erosqnative/nx240-av-sync.nxv` is a one-minute drift test. At every whole
second, its center square flashes white for exactly two video frames while an
80 ms stereo beep begins. The flash and beep should appear simultaneous near
the beginning and still be simultaneous near the end. Recreate it with:

```sh
./tools/make-av-sync-test.sh
```

## Endurance test

`dist/erosqnative/nx240-endurance.nxv` repeats the validated synchronization
stream for five minutes without re-encoding. It exercises larger MP4 sample
tables, repeated seeking, sustained A/V synchronization, and the final PCM
drain. Recreate it, optionally with a different duration in seconds, with:

```sh
./tools/make-endurance-video.sh
./tools/make-endurance-video.sh output.nxv 600
```

## Rockbox build

Obtain a current Rockbox source checkout and configured build directory, then:

```sh
./tools/stage-rockbox.sh /path/to/rockbox
make -C /path/to/rockbox/build
```

The staging script copies `plugin/` to `apps/plugins/mpegplayer_nx` and adds the
three current Rockbox integration entries. Re-running it updates the copy without
duplicating entries. The built viewer is expected at:

```text
.rockbox/rocks/viewers/mpegplayer_nx.rock
```

Select an `.nxv` file in Rockbox's file browser to launch it. NXV files remain
standard MP4 containers; the dedicated extension avoids Rockbox's built-in
`.mp4` AAC-audio handler, which takes precedence over viewer mappings.

During playback, Play pauses/resumes, Volume changes the Rockbox volume, Menu
shows the two-second progress OSD, and Previous/Next seek backward/forward by
ten seconds to the nearest usable keyframe. The backlight is forced on until
playback exits, after which the user's normal backlight setting is restored.

### Install a native test build manually

Copy `dist/erosqnative/mpegplayer_nx.rock` to this path on the player's SD card:

```text
/.rockbox/rocks/viewers/mpegplayer_nx.rock
```

Then append the two lines from `dist/erosqnative/viewers.config.snippet` to the
SD card's `/.rockbox/viewers.config`. Back up the original file first. Selecting
an `.nxv` in Rockbox will then launch NX.

Rockbox plugins are tied to both the firmware's plugin API and the exact player
target. The default artifact in `dist/erosqnative/` targets current Rockbox
source. Units running the Rockbox 4.0 stable release must instead use one of
these API 273 builds:

| Rockbox target | Plugin |
| --- | --- |
| `erosqnative` (original H2/Eros Q target) | `dist/erosqnative/rockbox-4.0/mpegplayer_nx.rock` |
| `erosqnative_v3` | `dist/erosqnative/rockbox-4.0/erosqnative_v3/mpegplayer_nx.rock` |
| `erosqnative_v4` | `dist/erosqnative/rockbox-4.0/erosqnative_v4/mpegplayer_nx.rock` |

The current M6 hardware-validated artifact is the original `erosqnative`
Rockbox 4.0 build. The smaller v3/v4 files are retained as earlier
target-identification builds and have not received the M6 playback validation.

The first file is also duplicated under the `erosqnative/` subdirectory for a
uniform three-target layout. Check **System > Rockbox Info** if the target is
uncertain. `Incompatible Version` means the plugin was found but its API does
not match the firmware; `Can't open ...mpegplayer_nx.rock` means Rockbox could
not load that file at all, so re-copy it and verify its path and size.

## Suggested NX240 encode

```sh
ffmpeg -i input.mkv \
  -map 0:v:0 -map 0:a:0 \
  -vf "scale=320:240:force_original_aspect_ratio=decrease:flags=lanczos,pad=320:240:(ow-iw)/2:(oh-ih)/2,setsar=1" \
  -c:v libx264 -profile:v baseline -level:v 1.3 \
  -pix_fmt yuv420p -refs 1 -bf 0 -g 48 \
  -b:v 500k -maxrate 700k -bufsize 1000k \
  -c:a aac -profile:a aac_low -ar 44100 -ac 2 -b:a 80k \
  -movflags +faststart -f mp4 output.nxv
```

## Source basis

The Rockbox-facing layout follows the current upstream `apps/plugins/SUBDIRS`,
`CATEGORIES`, `viewers.config`, per-plugin `SOURCES`, and `plugin_start()`
conventions. No MPEGPlayer source has been copied into this project.

## License

Project-owned code is GPL-2.0-or-later, matching Rockbox plugin development
conventions. The vendored `h264bsd` files retain their Apache-2.0 notices; see
`plugin/h264bsd/UPSTREAM.md`. A combined binary containing both is distributed
under GPL version 3 or later. See [LICENSE](LICENSE).
