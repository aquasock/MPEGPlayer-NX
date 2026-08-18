/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "nx_h264.h"

#include <stdlib.h>
#include <string.h>

#include "h264bsd/h264bsd_decoder.h"
#include "h264bsd/h264bsd_util.h"

#ifdef ROCKBOX
#include <tlsf.h>
#endif

#define NX_H264_MAX_NAL_SIZE (1024u * 1024u)

static void *nx_memory_pool;
static size_t nx_memory_size;
static size_t nx_memory_peak_bytes;

int nx_h264_memory_init(void *memory, size_t bytes)
{
    nx_memory_pool = memory;
    nx_memory_size = bytes;
    nx_memory_peak_bytes = 0;
#ifdef ROCKBOX
    if (memory == NULL || bytes < 4096)
        return -1;
    return init_memory_pool(bytes, memory) == (size_t)-1 ? -1 : 0;
#else
    (void)memory;
    (void)bytes;
    return 0;
#endif
}

void nx_h264_memory_destroy(void)
{
#ifdef ROCKBOX
    if (nx_memory_pool != NULL)
        destroy_memory_pool(nx_memory_pool);
#endif
    nx_memory_pool = NULL;
    nx_memory_size = 0;
}

size_t nx_h264_memory_used(void)
{
#ifdef ROCKBOX
    return nx_memory_pool == NULL ? 0 : get_used_size(nx_memory_pool);
#else
    return 0;
#endif
}

size_t nx_h264_memory_peak(void)
{
    return nx_memory_peak_bytes;
}

void *nx_h264_malloc(size_t bytes)
{
    void *pointer;
#ifdef ROCKBOX
    if (nx_memory_pool == NULL || bytes > nx_memory_size)
        return NULL;
    pointer = malloc_ex(bytes, nx_memory_pool);
#else
    pointer = malloc(bytes);
#endif
    if (pointer != NULL) {
        size_t used = nx_h264_memory_used();
        if (used > nx_memory_peak_bytes)
            nx_memory_peak_bytes = used;
    }
    return pointer;
}

void nx_h264_free(void *pointer)
{
    if (pointer == NULL)
        return;
#ifdef ROCKBOX
    free_ex(pointer, nx_memory_pool);
#else
    free(pointer);
#endif
}

static uint16_t read_be16(const unsigned char data[2])
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t read_be_n(const unsigned char *data, uint32_t bytes)
{
    uint32_t value = 0;
    uint32_t i;
    for (i = 0; i < bytes; ++i)
        value = (value << 8) | data[i];
    return value;
}

static enum nx_h264_status decoder_dimensions(struct nx_h264_decoder *decoder)
{
    storage_t *storage = decoder->storage;
    u32 cropping;
    u32 left;
    u32 top;
    u32 width;
    u32 height;

    decoder->coded_width = h264bsdPicWidth(storage) * 16u;
    decoder->coded_height = h264bsdPicHeight(storage) * 16u;
    h264bsdCroppingParams(storage, &cropping, &left, &width, &top, &height);
    if (!cropping) {
        left = top = 0;
        width = decoder->coded_width;
        height = decoder->coded_height;
    }

    if (width == 0 || height == 0 || width > 320 || height > 240 ||
        left + width > decoder->coded_width ||
        top + height > decoder->coded_height)
        return NX_H264_UNSUPPORTED_SIZE;

    decoder->crop_left = left;
    decoder->crop_top = top;
    decoder->width = width;
    decoder->height = height;
    return NX_H264_OK;
}

static enum nx_h264_status decode_buffer(struct nx_h264_decoder *decoder,
                                         unsigned char *nal, uint32_t bytes,
                                         int *replay)
{
    storage_t *storage = decoder->storage;
    enum nx_h264_status status = NX_H264_OK;
    u32 result;
    u32 consumed;

    *replay = 0;

    do {
        consumed = 0;
        result = h264bsdDecode(storage, nal, bytes,
                               decoder->samples_read, &consumed);
        if (result == H264BSD_HDRS_RDY) {
            status = decoder_dimensions(decoder);
            if (status != NX_H264_OK)
                break;
        } else if (result == H264BSD_PIC_RDY) {
            u32 is_idr;
            u32 picture_id;
            u32 error_mbs;
            decoder->picture = h264bsdNextOutputPicture(
                storage, &picture_id, &is_idr, &error_mbs);
            decoder->picture_id = picture_id;
            decoder->error_mbs = error_mbs;
            *replay = consumed == 0;
            if (decoder->width == 0)
                status = decoder_dimensions(decoder);
            if (decoder->picture == NULL && status == NX_H264_OK)
                status = NX_H264_DECODE_ERROR;
        } else if (result == H264BSD_ERROR ||
                   result == H264BSD_PARAM_SET_ERROR) {
            status = NX_H264_DECODE_ERROR;
        } else if (result == H264BSD_MEMALLOC_ERROR) {
            status = NX_H264_NO_MEMORY;
        }
    } while (result == H264BSD_HDRS_RDY && status == NX_H264_OK);

    return status;
}

static enum nx_h264_status decode_nal(struct nx_h264_decoder *decoder,
                                      const struct nx_reader *reader,
                                      uint64_t offset, uint32_t bytes)
{
    unsigned char *nal;
    enum nx_h264_status status;
    int replay;

    if (bytes == 0 || bytes > NX_H264_MAX_NAL_SIZE ||
        offset > reader->size || bytes > reader->size - offset)
        return NX_H264_BAD_CONFIG;

    nal = nx_h264_malloc(bytes);
    if (nal == NULL)
        return NX_H264_NO_MEMORY;
    if (reader->read_at(reader->context, offset, nal, bytes) != 0) {
        nx_h264_free(nal);
        return NX_H264_IO_ERROR;
    }
    status = decode_buffer(decoder, nal, bytes, &replay);
    nx_h264_free(nal);
    return status;
}

static enum nx_h264_status decode_avcc(struct nx_h264_decoder *decoder,
                                       const struct nx_reader *reader,
                                       const struct nx_mp4_track *track)
{
    unsigned char header[7];
    unsigned char length_data[2];
    uint64_t offset;
    uint64_t end;
    uint32_t count;
    uint32_t i;
    int parameter_type;

    if (track->avcc_size < sizeof(header) || track->avcc_data > reader->size ||
        track->avcc_size > reader->size - track->avcc_data ||
        reader->read_at(reader->context, track->avcc_data,
                        header, sizeof(header)) != 0)
        return NX_H264_BAD_CONFIG;
    if (header[0] != 1)
        return NX_H264_BAD_CONFIG;

    offset = track->avcc_data + 6;
    end = track->avcc_data + track->avcc_size;
    count = header[5] & 31u;

    for (parameter_type = 0; parameter_type < 2; ++parameter_type) {
        for (i = 0; i < count; ++i) {
            uint32_t bytes;
            enum nx_h264_status status;
            if (offset > end || end - offset < 2 ||
                reader->read_at(reader->context, offset,
                                length_data, sizeof(length_data)) != 0)
                return NX_H264_BAD_CONFIG;
            bytes = read_be16(length_data);
            offset += 2;
            if (bytes > end - offset)
                return NX_H264_BAD_CONFIG;
            status = decode_nal(decoder, reader, offset, bytes);
            if (status != NX_H264_OK)
                return status;
            offset += bytes;
        }

        if (parameter_type == 0) {
            unsigned char pps_count;
            if (offset >= end ||
                reader->read_at(reader->context, offset, &pps_count, 1) != 0)
                return NX_H264_BAD_CONFIG;
            count = pps_count;
            offset++;
        }
    }

    return NX_H264_OK;
}

enum nx_h264_status nx_h264_decoder_init(struct nx_h264_decoder *decoder)
{
    storage_t *storage;
    if (decoder == NULL)
        return NX_H264_BAD_CONFIG;
    memset(decoder, 0, sizeof(*decoder));
    storage = nx_h264_malloc(sizeof(*storage));
    if (storage == NULL)
        return NX_H264_NO_MEMORY;
    decoder->storage = storage;
    if (h264bsdInit(storage, HANTRO_TRUE) != HANTRO_OK) {
        nx_h264_free(storage);
        decoder->storage = NULL;
        return NX_H264_NO_MEMORY;
    }
    return NX_H264_OK;
}

void nx_h264_decoder_destroy(struct nx_h264_decoder *decoder)
{
    if (decoder != NULL && decoder->storage != NULL) {
        h264bsdShutdown(decoder->storage);
        nx_h264_free(decoder->pending_nal);
        nx_h264_free(decoder->storage);
        decoder->storage = NULL;
        decoder->picture = NULL;
        decoder->pending_nal = NULL;
    }
}

enum nx_h264_status nx_h264_stream_start_at(
    struct nx_h264_decoder *decoder, const struct nx_reader *reader,
    const struct nx_mp4_track *track, uint32_t sample_number)
{
    enum nx_mp4_status mp4_status;
    enum nx_h264_status status;
    int next;

    if (decoder == NULL || decoder->storage == NULL || reader == NULL ||
        track == NULL || track->avcc_size == 0 || decoder->stream_started)
        return NX_H264_BAD_CONFIG;

    status = decode_avcc(decoder, reader, track);
    if (status != NX_H264_OK)
        return status;

    mp4_status = nx_mp4_sample_iter_seek(&decoder->iterator, reader, track,
                                         sample_number);
    if (mp4_status != NX_MP4_OK)
        return NX_H264_BAD_CONFIG;

    next = nx_mp4_sample_iter_next(&decoder->iterator, &decoder->sample);
    if (next != 1)
        return next < 0 ? NX_H264_IO_ERROR : NX_H264_END_OF_STREAM;
    decoder->sample_cursor = decoder->sample.offset;
    decoder->sample_end = decoder->sample.offset + decoder->sample.size;
    if (decoder->sample_end < decoder->sample_cursor ||
        decoder->sample_end > reader->size)
        return NX_H264_BAD_CONFIG;
    decoder->sample_active = 1;
    decoder->samples_read = decoder->sample.number;

    decoder->reader = reader;
    decoder->track = track;
    decoder->stream_started = 1;
    return NX_H264_OK;
}

enum nx_h264_status nx_h264_stream_start(
    struct nx_h264_decoder *decoder, const struct nx_reader *reader,
    const struct nx_mp4_track *track)
{
    return nx_h264_stream_start_at(decoder, reader, track, 1);
}

enum nx_h264_status nx_h264_decode_next_picture(
    struct nx_h264_decoder *decoder)
{
    unsigned char length_data[4];

    if (decoder == NULL || !decoder->stream_started ||
        decoder->stream_ended || decoder->reader == NULL ||
        decoder->track == NULL)
        return decoder != NULL && decoder->stream_ended ?
            NX_H264_END_OF_STREAM : NX_H264_BAD_CONFIG;

    decoder->picture = NULL;
    while (!decoder->stream_ended) {
        unsigned char *nal;
        uint32_t bytes;
        enum nx_h264_status status;
        int replay;

        if (decoder->pending_nal != NULL) {
            nal = decoder->pending_nal;
            bytes = decoder->pending_nal_size;
            status = decode_buffer(decoder, nal, bytes, &replay);
            if (!replay) {
                nx_h264_free(decoder->pending_nal);
                decoder->pending_nal = NULL;
                decoder->pending_nal_size = 0;
            }
            if (status != NX_H264_OK)
                return status;
            if (decoder->picture != NULL)
                return NX_H264_OK;
            continue;
        }

        if (!decoder->sample_active ||
            decoder->sample_cursor >= decoder->sample_end) {
            int next = nx_mp4_sample_iter_next(&decoder->iterator,
                                               &decoder->sample);
            if (next < 0)
                return NX_H264_IO_ERROR;
            if (next == 0) {
                u32 picture_id;
                u32 is_idr;
                u32 error_mbs;
                h264bsdFlushBuffer(decoder->storage);
                decoder->picture = h264bsdNextOutputPicture(
                    decoder->storage, &picture_id, &is_idr, &error_mbs);
                decoder->stream_ended = decoder->picture == NULL;
                if (decoder->picture != NULL) {
                    decoder->picture_id = picture_id;
                    decoder->error_mbs = error_mbs;
                    return NX_H264_OK;
                }
                return NX_H264_END_OF_STREAM;
            }
            decoder->sample_cursor = decoder->sample.offset;
            decoder->sample_end = decoder->sample.offset + decoder->sample.size;
            if (decoder->sample_end < decoder->sample_cursor ||
                decoder->sample_end > decoder->reader->size)
                return NX_H264_BAD_CONFIG;
            decoder->sample_active = 1;
            decoder->samples_read++;
        }

        if (decoder->sample_end - decoder->sample_cursor <
                decoder->track->nal_length_size ||
            decoder->track->nal_length_size < 1 ||
            decoder->track->nal_length_size > 4 ||
            decoder->reader->read_at(decoder->reader->context,
                                     decoder->sample_cursor, length_data,
                                     decoder->track->nal_length_size) != 0)
            return NX_H264_BAD_CONFIG;
        bytes = read_be_n(length_data, decoder->track->nal_length_size);
        decoder->sample_cursor += decoder->track->nal_length_size;
        if (bytes == 0 || bytes > NX_H264_MAX_NAL_SIZE ||
            bytes > decoder->sample_end - decoder->sample_cursor)
            return NX_H264_BAD_CONFIG;

        nal = nx_h264_malloc(bytes);
        if (nal == NULL)
            return NX_H264_NO_MEMORY;
        if (decoder->reader->read_at(decoder->reader->context,
                                     decoder->sample_cursor, nal, bytes) != 0) {
            nx_h264_free(nal);
            return NX_H264_IO_ERROR;
        }
        decoder->sample_cursor += bytes;
        status = decode_buffer(decoder, nal, bytes, &replay);
        if (replay) {
            decoder->pending_nal = nal;
            decoder->pending_nal_size = bytes;
        } else {
            nx_h264_free(nal);
        }
        if (status != NX_H264_OK)
            return status;
        if (decoder->picture != NULL)
            return NX_H264_OK;
    }

    return NX_H264_END_OF_STREAM;
}

enum nx_h264_status nx_h264_decode_first_picture(
    struct nx_h264_decoder *decoder, const struct nx_reader *reader,
    const struct nx_mp4_track *track)
{
    enum nx_h264_status status = nx_h264_stream_start(decoder, reader, track);
    if (status != NX_H264_OK)
        return status;
    return nx_h264_decode_next_picture(decoder);
}

uint32_t nx_h264_picture_checksum(const struct nx_h264_decoder *decoder)
{
    uint32_t hash = 2166136261u;
    uint64_t bytes;
    uint64_t i;
    if (decoder == NULL || decoder->picture == NULL)
        return 0;
    bytes = (uint64_t)decoder->coded_width * decoder->coded_height * 3u / 2u;
    for (i = 0; i < bytes; ++i) {
        hash ^= decoder->picture[i];
        hash *= 16777619u;
    }
    return hash;
}

const char *nx_h264_status_string(enum nx_h264_status status)
{
    switch (status) {
    case NX_H264_OK: return "H.264 picture ready";
    case NX_H264_BAD_CONFIG: return "Bad AVC configuration";
    case NX_H264_IO_ERROR: return "Video read error";
    case NX_H264_NO_MEMORY: return "H.264 out of memory";
    case NX_H264_DECODE_ERROR: return "H.264 decode error";
    case NX_H264_NO_PICTURE: return "No decoded picture";
    case NX_H264_UNSUPPORTED_SIZE: return "Unsupported picture size";
    case NX_H264_END_OF_STREAM: return "End of video";
    default: return "Unknown H.264 error";
    }
}
