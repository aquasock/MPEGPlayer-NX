# Hardware validation

These results were measured on a physical HIFI WALKER H2 / AIGO Eros Q using
the original `erosqnative` target and the Rockbox 4.0 stable release. The
firmware reported Rockbox 4.0 and plugin API 273.

## Current build

- Rockbox source basis: tag `v4.0-final`, commit `e094c59`
- Rockbox target: `erosqnative` (target ID 116)
- Toolchain: `mipsel-elf-gcc` 9.5.0
- Plugin: `dist/erosqnative/rockbox-4.0/mpegplayer_nx.rock`
- Plugin size: 326,808 bytes
- SHA-256: `6a637c902696b5d58ad7120559a671f6541da1077d7c65da869ba35250e5c57d`

## Device results

| Test | Frames | Drops | Video avg/max | AAC avg/max | PCM underruns | Peak memory |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 20 s stress, early M3 | 480/480 | 0 | 11.4/20.1 ms | video-only | n/a | 320 KiB |
| 20 s stress, M4 A/V | 480/480 | 0 | 13.8/22.8 ms | 1.0/1.7 ms | 0 | 408 KiB |
| 60 s synchronization | 1440/1440 | 0 | 10.0/17.6 ms | 0.9/1.0 ms | 0 | 446 KiB |
| 5 min endurance, pre-tail fix | 7196/7197 | 0 | 11.0/19.6 ms | 0.9/1.1 ms | 0 | 447 KiB |
| 5 min final-frame retest | 7198/7198 | 0 | not re-recorded | not re-recorded | 0 | stable |

The M4 test produced clean stereo audio in both ears. On the one-minute sync
test, each beep remained aligned with its corresponding visual flash.

The following controls and behaviors were also exercised on the unit:

- pause and resume;
- live volume adjustment;
- ten-second Previous/Next keyframe seeking;
- playback Menu progress overlay;
- backlight held on during playback and restored on exit;
- clean end-of-file PCM drain;
- zero-drop playback of all three benchmark streams.

Progress-bar flicker was removed by reserving the overlay rows from video
refreshes and updating the overlay once per second. The transient `Seeking...`
panel was removed because it flashed between seek steps on the LCD.
Held Previous/Next repeat events are ignored after the initial ten-second seek,
preventing overlapping decoder restarts and corrupted frames.

## Test media checksums

| File | Purpose | SHA-256 |
| --- | --- | --- |
| `nx240-test.nxv` | Basic 320x240 correctness | `f3d27c2e7e94e85e4efc63ea843ebcb122cbcdb12181221b1e454d4c88a6809e` |
| `nx240-stress.nxv` | 20-second decode stress | `f5f6ca398b9137351e7077bec1a3c218ee5bdf215139d6ed815ae0db472f3213` |
| `nx240-av-sync.nxv` | One-minute flash/beep sync | `1179098f349d0390d67ebb45a7eab0063efec821a916b974244cadd8b88003ef` |
| `nx240-endurance.nxv` | Five-minute sustained playback | `1b775c941662e59f4cb5c99c93ff36b3772024fbcf3aaf54802819de89dbbe6d` |

The five-minute endurance run remained synchronized and stable, with no frame
drops or PCM underruns. Host decoding confirms all 7,198 video samples are
valid. The initial device run stopped one decoded picture early when the PCM clock ended
before the H.264 decoder flushed its final buffered pictures. The current build
adds a bounded 250 ms silent-tail clock to finish those pictures without
allowing badly truncated audio tracks to run on indefinitely. The physical-device
retest completed successfully with all 7,198 pictures displayed, zero drops,
and zero PCM underruns.
