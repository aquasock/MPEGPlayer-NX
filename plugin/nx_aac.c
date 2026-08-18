/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "plugin.h"
#include "nx_aac.h"
#include "nx_h264.h"

#include "libfaad/common.h"
#include "libfaad/structs.h"
#include "libfaad/decoder.h"

#undef malloc
#undef calloc
#undef realloc
#undef free
#undef strlen

#define NX_AAC_MAX_SAMPLE (8u * 1024u)
#define NX_AAC_OBJECT_TYPE_LC 2u
#define NX_PCM_RING_SIZE (128u * 1024u)
#define NX_PCM_FRAME_BYTES 4u
#define NX_PCM_CHUNK_SIZE 4096u
#define NX_PCM_CHANNEL PCM_MIXER_CHAN_PLAYBACK

#ifndef ALIGNED_ATTR
#define ALIGNED_ATTR(x) __attribute__((aligned(x)))
#endif

static struct nx_pcm_output *active_output;
static int16_t silence[256 * 2] ALIGNED_ATTR(4) = { 0 };

static uint32_t audio_precision_time(void)
{
#if CONFIG_CPU == X1000 && !defined(SIMULATOR)
    return __ost_read32();
#else
    return (uint32_t)*rb->current_tick;
#endif
}

static uint32_t audio_elapsed_us(uint32_t start)
{
    uint32_t elapsed = audio_precision_time() - start;
#if CONFIG_CPU == X1000 && !defined(SIMULATOR)
    return elapsed / OST_TICKS_PER_US;
#else
    return elapsed * (1000000u / HZ);
#endif
}

static int16_t fixed_to_s16(int32_t sample)
{
    if (sample >= 0) {
        sample += 1 << (REAL_BITS - 1);
        if (sample >= REAL_CONST(32767))
            return 32767;
    } else {
        sample -= 1 << (REAL_BITS - 1);
        if (sample <= REAL_CONST(-32768))
            return -32768;
    }
    return (int16_t)(sample >> REAL_BITS);
}

enum nx_aac_status nx_aac_decoder_init(
    struct nx_aac_decoder *decoder, const struct nx_reader *reader,
    const struct nx_mp4_track *track)
{
    unsigned char asc[64];
    NeAACDecHandle handle;
    NeAACDecConfigurationPtr config;
    uint32_t sample_rate = 0;
    uint8_t channels = 0;

    if (decoder == NULL || reader == NULL || track == NULL ||
        track->asc_size < 2 || track->asc_size > sizeof(asc) ||
        track->asc_data > reader->size ||
        track->asc_size > reader->size - track->asc_data)
        return NX_AAC_BAD_CONFIG;

    rb->memset(decoder, 0, sizeof(*decoder));
    if (reader->read_at(reader->context, track->asc_data,
                        asc, track->asc_size) != 0)
        return NX_AAC_IO_ERROR;

    handle = NeAACDecOpen();
    if (handle == NULL)
        return NX_AAC_NO_MEMORY;
    config = NeAACDecGetCurrentConfiguration(handle);
    config->defObjectType = NX_AAC_OBJECT_TYPE_LC;
    config->defSampleRate = track->sample_rate;
    config->outputFormat = FAAD_FMT_FIXED;
    if (!NeAACDecSetConfiguration(handle, config))
        return NX_AAC_LIBRARY_CONFIG;
    if (NeAACDecInit2(handle, asc, track->asc_size,
                      &sample_rate, &channels) != 0)
        return NX_AAC_ASC_REJECTED;
    if (sample_rate != track->sample_rate || channels == 0 || channels > 2)
        return NX_AAC_FORMAT_MISMATCH;
    if (nx_mp4_sample_iter_init(&decoder->iterator, reader, track) != NX_MP4_OK)
        return NX_AAC_INDEX_ERROR;

    decoder->pcm = nx_h264_malloc(1024u * 2u * sizeof(*decoder->pcm));
    if (decoder->pcm == NULL)
        return NX_AAC_NO_MEMORY;
    decoder->handle = handle;
    decoder->reader = reader;
    decoder->track = track;
    decoder->sample_rate = sample_rate;
    decoder->channels = channels;
    return NX_AAC_OK;
}

void nx_aac_decoder_destroy(struct nx_aac_decoder *decoder)
{
    if (decoder != NULL) {
        nx_h264_free(decoder->pcm);
        decoder->pcm = NULL;
        decoder->handle = NULL;
    }
}

enum nx_aac_status nx_aac_decode_next(struct nx_aac_decoder *decoder,
                                      uint32_t *pcm_frames)
{
    struct nx_mp4_sample sample;
    unsigned char *compressed;
    NeAACDecFrameInfo info;
    NeAACDecHandle handle;
    uint32_t start;
    uint32_t elapsed;
    uint32_t frames;
    unsigned i;
    int next;

    if (decoder == NULL || decoder->handle == NULL || pcm_frames == NULL)
        return NX_AAC_BAD_CONFIG;
    *pcm_frames = 0;
    next = nx_mp4_sample_iter_next(&decoder->iterator, &sample);
    if (next == 0)
        return NX_AAC_END_OF_STREAM;
    if (next < 0 || sample.size == 0 || sample.size > NX_AAC_MAX_SAMPLE)
        return NX_AAC_BAD_CONFIG;

    compressed = nx_h264_malloc(sample.size);
    if (compressed == NULL)
        return NX_AAC_NO_MEMORY;
    if (decoder->reader->read_at(decoder->reader->context, sample.offset,
                                 compressed, sample.size) != 0) {
        nx_h264_free(compressed);
        return NX_AAC_IO_ERROR;
    }

    start = audio_precision_time();
    handle = decoder->handle;
    if (NeAACDecDecode(handle, &info, compressed, sample.size) == NULL ||
        info.error != 0) {
        nx_h264_free(compressed);
        return NX_AAC_DECODE_ERROR;
    }
    elapsed = audio_elapsed_us(start);
    nx_h264_free(compressed);
    decoder->decoded_frames++;
    decoder->decode_us += elapsed;
    if (elapsed > decoder->worst_decode_us)
        decoder->worst_decode_us = elapsed;

    if (info.samples == 0)
        return NX_AAC_OK;
    if (info.channels == 0 || info.channels > 2 ||
        info.samples % info.channels != 0)
        return NX_AAC_DECODE_ERROR;
    frames = info.samples / info.channels;
    if (frames > 1024)
        return NX_AAC_DECODE_ERROR;

    for (i = 0; i < frames; ++i) {
        int32_t left = handle->time_out[handle->internal_channel[0]][i];
        int32_t right = info.channels == 1 ? left :
            handle->time_out[handle->internal_channel[1]][i];
        decoder->pcm[i * 2] = fixed_to_s16(left);
        decoder->pcm[i * 2 + 1] = fixed_to_s16(right);
    }
    *pcm_frames = frames;
    return NX_AAC_OK;
}

enum nx_aac_status nx_aac_decoder_seek(struct nx_aac_decoder *decoder,
                                       uint32_t sample_number)
{
    if (decoder == NULL || decoder->reader == NULL || decoder->track == NULL)
        return NX_AAC_BAD_CONFIG;
    return nx_mp4_sample_iter_seek(&decoder->iterator, decoder->reader,
                                   decoder->track, sample_number) == NX_MP4_OK ?
           NX_AAC_OK : NX_AAC_INDEX_ERROR;
}

static void pcm_get_more(const void **start, size_t *size)
{
    struct nx_pcm_output *output = active_output;
    uint32_t used;
    uint32_t position;
    uint32_t contiguous;

    if (output == NULL) {
        *start = silence;
        *size = sizeof(silence);
        return;
    }

    output->read_bytes += output->current_bytes;
    output->current_bytes = 0;
    used = output->written_bytes - output->read_bytes;
    if (used < NX_PCM_FRAME_BYTES) {
        if (output->end_of_stream) {
            *start = NULL;
            *size = 0;
            return;
        }
        output->underruns++;
        output->submitted_frames += sizeof(silence) / NX_PCM_FRAME_BYTES;
        *start = silence;
        *size = sizeof(silence);
        return;
    }

    position = output->read_bytes & (output->capacity - 1);
    contiguous = output->capacity - position;
    if (contiguous > used)
        contiguous = used;
    if (contiguous > NX_PCM_CHUNK_SIZE)
        contiguous = NX_PCM_CHUNK_SIZE;
    contiguous &= ~(NX_PCM_FRAME_BYTES - 1);
    output->current_bytes = contiguous;
    output->submitted_frames += contiguous / NX_PCM_FRAME_BYTES;
    *start = output->ring + position;
    *size = contiguous;
}

int nx_pcm_output_init(struct nx_pcm_output *output, uint32_t sample_rate)
{
    if (output == NULL || (sample_rate != 44100 && sample_rate != 48000))
        return -1;
    rb->memset(output, 0, sizeof(*output));
    output->ring = nx_h264_malloc(NX_PCM_RING_SIZE);
    if (output->ring == NULL)
        return -1;
    output->capacity = NX_PCM_RING_SIZE;
    output->sample_rate = sample_rate;
    return 0;
}

void nx_pcm_output_destroy(struct nx_pcm_output *output)
{
    if (output != NULL) {
        nx_pcm_output_stop(output);
        nx_h264_free(output->ring);
        output->ring = NULL;
    }
}

uint32_t nx_pcm_output_used(const struct nx_pcm_output *output)
{
    return output->written_bytes - output->read_bytes;
}

uint32_t nx_pcm_output_free(const struct nx_pcm_output *output)
{
    return output->capacity - nx_pcm_output_used(output);
}

int nx_pcm_output_write(struct nx_pcm_output *output, const int16_t *pcm,
                        uint32_t frames)
{
    uint32_t bytes = frames * NX_PCM_FRAME_BYTES;
    uint32_t position;
    uint32_t first;
    if (output == NULL || pcm == NULL || bytes > nx_pcm_output_free(output))
        return -1;
    position = output->written_bytes & (output->capacity - 1);
    first = output->capacity - position;
    if (first > bytes)
        first = bytes;
    rb->memcpy(output->ring + position, pcm, first);
    if (first < bytes)
        rb->memcpy(output->ring, (const unsigned char *)pcm + first,
                   bytes - first);
    output->written_bytes += bytes;
    return 0;
}

void nx_pcm_output_start(struct nx_pcm_output *output)
{
    if (output == NULL || output->started)
        return;
    rb->audio_stop();
#if INPUT_SRC_CAPS != 0
    rb->audio_set_input_source(AUDIO_SRC_PLAYBACK, SRCF_PLAYBACK);
    rb->audio_set_output_source(AUDIO_SRC_PLAYBACK);
#endif
    output->old_sample_rate = rb->mixer_get_frequency();
    rb->mixer_set_frequency(output->sample_rate);
    rb->mixer_channel_set_amplitude(NX_PCM_CHANNEL, MIX_AMP_UNITY);
    active_output = output;
    output->started = 1;
    rb->mixer_channel_play_data(NX_PCM_CHANNEL, pcm_get_more, NULL, 0);
}

void nx_pcm_output_pause(struct nx_pcm_output *output, int play)
{
    if (output != NULL && output->started)
        rb->mixer_channel_play_pause(NX_PCM_CHANNEL, play != 0);
}

void nx_pcm_output_stop(struct nx_pcm_output *output)
{
    if (output == NULL || !output->started)
        return;
    rb->mixer_channel_stop(NX_PCM_CHANNEL);
    active_output = NULL;
    output->started = 0;
    if (output->old_sample_rate != 0)
        rb->mixer_set_frequency(output->old_sample_rate);
}

void nx_pcm_output_reset(struct nx_pcm_output *output, uint64_t clock_base_us)
{
    if (output == NULL)
        return;
    nx_pcm_output_stop(output);
    output->read_bytes = 0;
    output->written_bytes = 0;
    output->submitted_frames = 0;
    output->current_bytes = 0;
    output->end_of_stream = 0;
    output->clock_base_us = clock_base_us;
}

void nx_pcm_output_mark_end(struct nx_pcm_output *output)
{
    if (output != NULL)
        output->end_of_stream = 1;
}

int nx_pcm_output_finished(const struct nx_pcm_output *output)
{
    return output != NULL && output->end_of_stream &&
           nx_pcm_output_used(output) == 0 &&
           rb->mixer_channel_status(NX_PCM_CHANNEL) == CHANNEL_STOPPED;
}

uint64_t nx_pcm_output_clock_us(const struct nx_pcm_output *output)
{
    uint32_t submitted;
    uint32_t waiting;
    if (output == NULL || output->sample_rate == 0)
        return 0;
    do {
        submitted = output->submitted_frames;
        waiting = rb->mixer_channel_get_bytes_waiting(NX_PCM_CHANNEL) /
                  NX_PCM_FRAME_BYTES;
    } while (submitted != output->submitted_frames ||
             (waiting == 0 &&
              rb->mixer_channel_status(NX_PCM_CHANNEL) == CHANNEL_PLAYING));
    if (waiting > submitted)
        waiting = submitted;
    return output->clock_base_us +
           (uint64_t)(submitted - waiting) * 1000000u / output->sample_rate;
}

const char *nx_aac_status_string(enum nx_aac_status status)
{
    switch (status) {
    case NX_AAC_OK: return "AAC OK";
    case NX_AAC_END_OF_STREAM: return "AAC end of stream";
    case NX_AAC_BAD_CONFIG: return "Unsupported AAC configuration";
    case NX_AAC_LIBRARY_CONFIG: return "AAC library setup failed";
    case NX_AAC_ASC_REJECTED: return "AAC AudioSpecificConfig rejected";
    case NX_AAC_FORMAT_MISMATCH: return "AAC rate/channel mismatch";
    case NX_AAC_INDEX_ERROR: return "AAC sample index failed";
    case NX_AAC_IO_ERROR: return "AAC file read failed";
    case NX_AAC_NO_MEMORY: return "Not enough AAC memory";
    case NX_AAC_DECODE_ERROR: return "AAC decode failed";
    default: return "Unknown AAC error";
    }
}
