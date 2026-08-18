/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MPEGPLAYER_NX_H264_H
#define MPEGPLAYER_NX_H264_H

#include <stddef.h>
#include <stdint.h>

#include "nx_mp4.h"

enum nx_h264_status {
    NX_H264_OK = 0,
    NX_H264_BAD_CONFIG,
    NX_H264_IO_ERROR,
    NX_H264_NO_MEMORY,
    NX_H264_DECODE_ERROR,
    NX_H264_NO_PICTURE,
    NX_H264_UNSUPPORTED_SIZE,
    NX_H264_END_OF_STREAM
};

struct nx_h264_decoder {
    void *storage;
    unsigned char *picture;
    uint32_t coded_width;
    uint32_t coded_height;
    uint32_t crop_left;
    uint32_t crop_top;
    uint32_t width;
    uint32_t height;
    uint32_t picture_id;
    uint32_t error_mbs;
    uint32_t samples_read;
    const struct nx_reader *reader;
    const struct nx_mp4_track *track;
    struct nx_mp4_sample_iter iterator;
    struct nx_mp4_sample sample;
    uint64_t sample_cursor;
    uint64_t sample_end;
    unsigned char *pending_nal;
    uint32_t pending_nal_size;
    int sample_active;
    int stream_started;
    int stream_ended;
};

int nx_h264_memory_init(void *memory, size_t bytes);
void nx_h264_memory_destroy(void);
size_t nx_h264_memory_used(void);
size_t nx_h264_memory_peak(void);

void *nx_h264_malloc(size_t bytes);
void nx_h264_free(void *pointer);

enum nx_h264_status nx_h264_decoder_init(struct nx_h264_decoder *decoder);
void nx_h264_decoder_destroy(struct nx_h264_decoder *decoder);

enum nx_h264_status nx_h264_decode_first_picture(
    struct nx_h264_decoder *decoder, const struct nx_reader *reader,
    const struct nx_mp4_track *track);

enum nx_h264_status nx_h264_stream_start(
    struct nx_h264_decoder *decoder, const struct nx_reader *reader,
    const struct nx_mp4_track *track);
enum nx_h264_status nx_h264_stream_start_at(
    struct nx_h264_decoder *decoder, const struct nx_reader *reader,
    const struct nx_mp4_track *track, uint32_t sample_number);
enum nx_h264_status nx_h264_decode_next_picture(
    struct nx_h264_decoder *decoder);

uint32_t nx_h264_picture_checksum(const struct nx_h264_decoder *decoder);
const char *nx_h264_status_string(enum nx_h264_status status);

#endif
