# Architecture

NX is split into media, playback, and platform layers so parsers and scheduling
can be tested on a workstation while the Rockbox boundary stays small.

```text
Rockbox viewer entry point
        |
        v
portable I/O + bounded allocator
        |
        v
MP4 demuxer --> compressed video queue --> H.264 decoder --> YUV420 --> LCD blitter
        |
        +-----> compressed audio queue --> AAC-LC decoder --> PCM ring buffer
                                                               |
                                                               v
                                                        master media clock
```

## Design constraints

1. No unbounded allocation while playing.
2. Audio is the master clock after PCM starts.
3. Late video is dropped; audio is never stalled to display a frame.
4. File reads are sequential except for startup, seeking, and index access.
5. Parsing uses checked 64-bit offsets even though practical NX240 files are
   expected to be much smaller than 4 GiB.
6. The H.264 decoder must be benchmarked separately before UI work expands.

## Milestones

### M0: boilerplate (complete)

- Rockbox viewer registration
- portable MP4 top-level probe
- host tests

### M1: MP4 index (complete)

- parse one `vide` and one `soun` track
- accept only `avc1` + `mp4a`
- parse `avcC` metadata
- implement `stts`, `stsc`, `stsz`, `stco`/`co64`, and `stss`
- expose timestamped compressed samples through bounded iterators

AAC AudioSpecificConfig parsing is implemented for AAC-LC mono/stereo tracks.

### M2: video benchmark (complete)

- integrate the integer-oriented `h264bsd` H.264 Baseline decoder
- decode `avcC` parameter sets and length-prefixed AVC samples
- verify the reference first frame against FFmpeg and report a checksum
- decode and display the first picture on the target
- verify first-frame correctness and decoder memory use on the target

### M3: silent video output (complete)

- use Rockbox's native YUV420-to-framebuffer blitter
- full-screen and letterboxed paths
- video-clock pacing and late-frame dropping
- pause/resume and exit controls
- report average/worst decode time, high-water memory, and dropped frames
- benchmark both a reference correctness clip and a high-detail motion clip
- use the X1000 OST hardware counter for sub-millisecond decode measurements
- verified on Eros Q native: 480/480 frames, zero drops, 11.4 ms average,
  20.1 ms worst-case H.264 decode time, and 320 KiB peak allocation

### M4: audio and synchronization (complete)

- AAC-LC decode to Rockbox PCM
- audio-clock synchronization
- pause/resume and underrun handling
- bounded 128 KiB PCM ring with audio decode timing statistics
- playback-scoped backlight override with normal settings restored on exit

Validated on Eros Q native with synchronized stereo output, zero dropped video
frames, and zero PCM underruns over the one-minute A/V sync test.

### M5: player controls (complete)

- elapsed/total-time overlay and progress bar
- dedicated volume-button handling
- keyframe-based backward/forward seeking
- stable OSD rendering and playback-scoped backlight override

### M6: robustness and endurance (in progress)

- drain buffered AAC cleanly after the final video frame
- stop cleanly if a damaged or mismatched file ends audio early
- reject unsupported NX240 dimensions, profile, level, frame rate, or audio
  format before decoder allocation, with a specific on-screen explanation
- five-minute fast-start endurance and synchronization asset

## Decoder selection gate

Do not select a decoder solely by desktop benchmarks. Candidates must be
license-compatible, build without an OS or floating-point dependency, fit the
plugin buffer, and perform well on the XBurst/MIPS target. The M2 harness is the
decision mechanism.
