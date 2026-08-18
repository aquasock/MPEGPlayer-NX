/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MPEGPLAYER_NX_AAC_H
#define MPEGPLAYER_NX_AAC_H

#include <stddef.h>
#include <stdint.h>

#include "nx_mp4.h"

enum nx_aac_status {
    NX_AAC_OK = 0,
    NX_AAC_END_OF_STREAM,
    NX_AAC_BAD_CONFIG,
    NX_AAC_LIBRARY_CONFIG,
    NX_AAC_ASC_REJECTED,
    NX_AAC_FORMAT_MISMATCH,
    NX_AAC_INDEX_ERROR,
    NX_AAC_IO_ERROR,
    NX_AAC_NO_MEMORY,
    NX_AAC_DECODE_ERROR
};

struct nx_aac_decoder {
    void *handle;
    const struct nx_reader *reader;
    const struct nx_mp4_track *track;
    struct nx_mp4_sample_iter iterator;
    int16_t *pcm;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t decoded_frames;
    uint64_t decode_us;
    uint32_t worst_decode_us;
};

struct nx_pcm_output {
    unsigned char *ring;
    uint32_t capacity;
    volatile uint32_t read_bytes;
    volatile uint32_t written_bytes;
    volatile uint32_t submitted_frames;
    volatile uint32_t current_bytes;
    volatile uint32_t underruns;
    volatile int end_of_stream;
    uint32_t sample_rate;
    uint64_t clock_base_us;
    unsigned int old_sample_rate;
    int started;
};

enum nx_aac_status nx_aac_decoder_init(
    struct nx_aac_decoder *decoder, const struct nx_reader *reader,
    const struct nx_mp4_track *track);
void nx_aac_decoder_destroy(struct nx_aac_decoder *decoder);
enum nx_aac_status nx_aac_decode_next(struct nx_aac_decoder *decoder,
                                      uint32_t *pcm_frames);
enum nx_aac_status nx_aac_decoder_seek(struct nx_aac_decoder *decoder,
                                      uint32_t sample_number);
const char *nx_aac_status_string(enum nx_aac_status status);

int nx_pcm_output_init(struct nx_pcm_output *output, uint32_t sample_rate);
void nx_pcm_output_destroy(struct nx_pcm_output *output);
uint32_t nx_pcm_output_used(const struct nx_pcm_output *output);
uint32_t nx_pcm_output_free(const struct nx_pcm_output *output);
int nx_pcm_output_write(struct nx_pcm_output *output, const int16_t *pcm,
                        uint32_t frames);
void nx_pcm_output_start(struct nx_pcm_output *output);
void nx_pcm_output_pause(struct nx_pcm_output *output, int play);
void nx_pcm_output_stop(struct nx_pcm_output *output);
void nx_pcm_output_reset(struct nx_pcm_output *output, uint64_t clock_base_us);
void nx_pcm_output_mark_end(struct nx_pcm_output *output);
int nx_pcm_output_finished(const struct nx_pcm_output *output);
uint64_t nx_pcm_output_clock_us(const struct nx_pcm_output *output);

#endif
