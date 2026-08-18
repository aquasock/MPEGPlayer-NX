/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MPEGPLAYER_NX_MP4_H
#define MPEGPLAYER_NX_MP4_H

#include <stddef.h>
#include <stdint.h>

typedef int (*nx_read_at_fn)(void *context, uint64_t offset,
                             void *destination, size_t bytes);

struct nx_reader {
    void *context;
    nx_read_at_fn read_at;
    uint64_t size;
};

enum nx_mp4_status {
    NX_MP4_OK = 0,
    NX_MP4_IO_ERROR,
    NX_MP4_NOT_ISOBMFF,
    NX_MP4_MALFORMED,
    NX_MP4_MISSING_MOOV,
    NX_MP4_MISSING_MDAT,
    NX_MP4_UNSUPPORTED,
    NX_MP4_MISSING_VIDEO
};

enum nx_profile_status {
    NX_PROFILE_OK = 0,
    NX_PROFILE_MISSING_AUDIO,
    NX_PROFILE_VIDEO_SIZE,
    NX_PROFILE_VIDEO_CODEC,
    NX_PROFILE_VIDEO_RATE,
    NX_PROFILE_AUDIO_FORMAT
};

struct nx_mp4_info {
    char major_brand[5];
    uint32_t minor_version;
    uint32_t top_level_box_count;
    uint64_t moov_offset;
    uint64_t moov_size;
    uint64_t moov_header_size;
    uint64_t mdat_offset;
    int has_ftyp;
    int has_moov;
    int has_mdat;
    int fast_start;
};

enum nx_track_type {
    NX_TRACK_UNKNOWN = 0,
    NX_TRACK_VIDEO,
    NX_TRACK_AUDIO
};

struct nx_mp4_track {
    enum nx_track_type type;
    char codec[5];
    uint32_t timescale;
    uint64_t duration;
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    uint32_t sample_rate;
    uint64_t asc_data;
    uint32_t asc_size;

    uint8_t avc_profile;
    uint8_t avc_compatibility;
    uint8_t avc_level;
    uint8_t nal_length_size;
    uint64_t avcc_data;
    uint32_t avcc_size;

    uint32_t sample_count;
    uint32_t sync_sample_count;
    uint32_t chunk_count;
    uint32_t stts_entry_count;
    uint32_t stsc_entry_count;
    uint32_t constant_sample_size;

    uint64_t stts_entries;
    uint64_t stsc_entries;
    uint64_t stsz_entries;
    uint64_t chunk_entries;
    uint64_t stss_entries;
    int chunk_offsets_64;
    int present;
    int complete;
};

struct nx_mp4_movie {
    struct nx_mp4_info file;
    struct nx_mp4_track video;
    struct nx_mp4_track audio;
};

struct nx_mp4_sample {
    uint64_t offset;
    uint64_t dts;
    uint32_t size;
    uint32_t duration;
    uint32_t number;
    int is_sync;
};

struct nx_mp4_sample_iter {
    const struct nx_reader *reader;
    const struct nx_mp4_track *track;
    uint32_t sample_index;
    uint32_t chunk_index;
    uint32_t sample_in_chunk;
    uint32_t samples_per_chunk;
    uint32_t stsc_index;
    uint32_t next_stsc_chunk;
    uint32_t stts_index;
    uint32_t stts_samples_left;
    uint32_t stts_delta;
    uint32_t sync_index;
    uint32_t next_sync_sample;
    uint64_t sample_offset;
    uint64_t dts;
};

enum nx_mp4_status nx_mp4_probe(const struct nx_reader *reader,
                                struct nx_mp4_info *info);
enum nx_mp4_status nx_mp4_parse(const struct nx_reader *reader,
                                struct nx_mp4_movie *movie);
enum nx_profile_status nx_mp4_validate_nx240(
    const struct nx_mp4_movie *movie);

enum nx_mp4_status nx_mp4_sample_iter_init(
    struct nx_mp4_sample_iter *iter, const struct nx_reader *reader,
    const struct nx_mp4_track *track);
enum nx_mp4_status nx_mp4_sample_iter_seek(
    struct nx_mp4_sample_iter *iter, const struct nx_reader *reader,
    const struct nx_mp4_track *track, uint32_t sample_number);

/* Find the sample at or before target_dts. If sync_only is nonzero, choose
 * the nearest preceding sync sample. Sample numbers are one-based. */
enum nx_mp4_status nx_mp4_find_sample_at_or_before(
    const struct nx_reader *reader, const struct nx_mp4_track *track,
    uint64_t target_dts, int sync_only, uint32_t *sample_number,
    uint64_t *sample_dts);

/* 1 = sample, 0 = end, -1 = malformed input or I/O failure. */
int nx_mp4_sample_iter_next(struct nx_mp4_sample_iter *iter,
                            struct nx_mp4_sample *sample);

const char *nx_mp4_status_string(enum nx_mp4_status status);

#endif
