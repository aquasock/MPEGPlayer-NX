/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "nx_mp4.h"

#include <string.h>

#define NX_BOX_HEADER_SIZE 8u
#define NX_EXTENDED_BOX_HEADER_SIZE 16u
#define NX_MAX_BOXES_PER_LEVEL 4096u

struct nx_box {
    uint64_t offset;
    uint64_t size;
    uint64_t header_size;
    uint64_t payload;
    uint64_t end;
    unsigned char type[4];
};

static uint16_t read_be16(const unsigned char *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t read_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t read_be64(const unsigned char *p)
{
    return ((uint64_t)read_be32(p) << 32) | read_be32(p + 4);
}

static int read_data(const struct nx_reader *reader, uint64_t offset,
                     void *destination, size_t bytes)
{
    if (reader == NULL || reader->read_at == NULL ||
        offset > reader->size || bytes > reader->size - offset)
        return -1;
    return reader->read_at(reader->context, offset, destination, bytes);
}

static int read_u32(const struct nx_reader *reader, uint64_t offset,
                    uint32_t *value)
{
    unsigned char data[4];
    if (read_data(reader, offset, data, sizeof(data)) != 0)
        return -1;
    *value = read_be32(data);
    return 0;
}

static int read_u64(const struct nx_reader *reader, uint64_t offset,
                    uint64_t *value)
{
    unsigned char data[8];
    if (read_data(reader, offset, data, sizeof(data)) != 0)
        return -1;
    *value = read_be64(data);
    return 0;
}

static int type_is(const unsigned char type[4], const char expected[4])
{
    return memcmp(type, expected, 4) == 0;
}

static enum nx_mp4_status read_box(const struct nx_reader *reader,
                                   uint64_t offset, uint64_t limit,
                                   struct nx_box *box)
{
    unsigned char header[NX_EXTENDED_BOX_HEADER_SIZE];
    uint32_t size32;
    uint64_t size;
    uint64_t header_size = NX_BOX_HEADER_SIZE;

    if (offset > limit || limit > reader->size ||
        limit - offset < NX_BOX_HEADER_SIZE)
        return NX_MP4_MALFORMED;
    if (read_data(reader, offset, header, NX_BOX_HEADER_SIZE) != 0)
        return NX_MP4_IO_ERROR;

    size32 = read_be32(header);
    size = size32;
    if (size32 == 1) {
        if (limit - offset < NX_EXTENDED_BOX_HEADER_SIZE)
            return NX_MP4_MALFORMED;
        if (read_data(reader, offset + NX_BOX_HEADER_SIZE,
                      header + NX_BOX_HEADER_SIZE,
                      NX_BOX_HEADER_SIZE) != 0)
            return NX_MP4_IO_ERROR;
        size = read_be64(header + NX_BOX_HEADER_SIZE);
        header_size = NX_EXTENDED_BOX_HEADER_SIZE;
    } else if (size32 == 0) {
        size = limit - offset;
    }

    if (size < header_size || size > limit - offset)
        return NX_MP4_MALFORMED;

    box->offset = offset;
    box->size = size;
    box->header_size = header_size;
    box->payload = offset + header_size;
    box->end = offset + size;
    memcpy(box->type, header + 4, 4);
    return NX_MP4_OK;
}

enum nx_mp4_status nx_mp4_probe(const struct nx_reader *reader,
                                struct nx_mp4_info *info)
{
    uint64_t offset = 0;

    if (reader == NULL || reader->read_at == NULL || info == NULL)
        return NX_MP4_IO_ERROR;

    memset(info, 0, sizeof(*info));
    info->moov_offset = UINT64_MAX;
    info->mdat_offset = UINT64_MAX;

    while (offset < reader->size) {
        struct nx_box box;
        enum nx_mp4_status status;

        if (info->top_level_box_count >= NX_MAX_BOXES_PER_LEVEL)
            return NX_MP4_MALFORMED;
        status = read_box(reader, offset, reader->size, &box);
        if (status != NX_MP4_OK)
            return status;

        if (type_is(box.type, "ftyp")) {
            unsigned char ftyp[8];
            if (box.end - box.payload < sizeof(ftyp) ||
                read_data(reader, box.payload, ftyp, sizeof(ftyp)) != 0)
                return NX_MP4_MALFORMED;
            memcpy(info->major_brand, ftyp, 4);
            info->major_brand[4] = '\0';
            info->minor_version = read_be32(ftyp + 4);
            info->has_ftyp = 1;
        } else if (type_is(box.type, "moov")) {
            info->has_moov = 1;
            info->moov_offset = box.offset;
            info->moov_size = box.size;
            info->moov_header_size = box.header_size;
        } else if (type_is(box.type, "mdat")) {
            info->has_mdat = 1;
            info->mdat_offset = box.offset;
        }

        info->top_level_box_count++;
        offset = box.end;
    }

    if (!info->has_ftyp)
        return NX_MP4_NOT_ISOBMFF;
    if (!info->has_moov)
        return NX_MP4_MISSING_MOOV;
    if (!info->has_mdat)
        return NX_MP4_MISSING_MDAT;

    info->fast_start = info->moov_offset < info->mdat_offset;
    return NX_MP4_OK;
}

static enum nx_mp4_status parse_mdhd(const struct nx_reader *reader,
                                     const struct nx_box *box,
                                     struct nx_mp4_track *track)
{
    unsigned char data[32];
    size_t need;

    if (box->end - box->payload < 20)
        return NX_MP4_MALFORMED;
    if (read_data(reader, box->payload, data, 4) != 0)
        return NX_MP4_IO_ERROR;

    need = data[0] == 1 ? 32u : 20u;
    if (box->end - box->payload < need ||
        read_data(reader, box->payload, data, need) != 0)
        return NX_MP4_MALFORMED;

    if (data[0] == 1) {
        track->timescale = read_be32(data + 20);
        track->duration = read_be64(data + 24);
    } else if (data[0] == 0) {
        track->timescale = read_be32(data + 12);
        track->duration = read_be32(data + 16);
    } else {
        return NX_MP4_UNSUPPORTED;
    }

    return track->timescale == 0 ? NX_MP4_MALFORMED : NX_MP4_OK;
}

static enum nx_mp4_status parse_hdlr(const struct nx_reader *reader,
                                     const struct nx_box *box,
                                     struct nx_mp4_track *track)
{
    unsigned char data[12];
    if (box->end - box->payload < sizeof(data) ||
        read_data(reader, box->payload, data, sizeof(data)) != 0)
        return NX_MP4_MALFORMED;
    if (memcmp(data + 8, "vide", 4) == 0)
        track->type = NX_TRACK_VIDEO;
    else if (memcmp(data + 8, "soun", 4) == 0)
        track->type = NX_TRACK_AUDIO;
    return NX_MP4_OK;
}

static enum nx_mp4_status parse_avcc(const struct nx_reader *reader,
                                     const struct nx_box *box,
                                     struct nx_mp4_track *track)
{
    unsigned char data[5];
    if (box->end - box->payload < sizeof(data) ||
        read_data(reader, box->payload, data, sizeof(data)) != 0)
        return NX_MP4_MALFORMED;
    if (data[0] != 1)
        return NX_MP4_UNSUPPORTED;
    track->avc_profile = data[1];
    track->avc_compatibility = data[2];
    track->avc_level = data[3];
    track->nal_length_size = (uint8_t)((data[4] & 3u) + 1u);
    if (box->end - box->payload > UINT32_MAX)
        return NX_MP4_UNSUPPORTED;
    track->avcc_data = box->payload;
    track->avcc_size = (uint32_t)(box->end - box->payload);
    return NX_MP4_OK;
}

static enum nx_mp4_status parse_esds(const struct nx_reader *reader,
                                     const struct nx_box *box,
                                     struct nx_mp4_track *track)
{
    unsigned char data[256];
    size_t bytes;
    size_t i;

    if (box->end - box->payload < 6)
        return NX_MP4_MALFORMED;
    bytes = (size_t)(box->end - box->payload);
    if (bytes > sizeof(data))
        bytes = sizeof(data);
    if (read_data(reader, box->payload, data, bytes) != 0)
        return NX_MP4_IO_ERROR;

    /* Skip the FullBox header and locate DecoderSpecificInfo (tag 0x05).
     * Descriptor lengths use up to four base-128 bytes. */
    for (i = 4; i + 3 <= bytes; ++i) {
        size_t cursor;
        size_t length = 0;
        unsigned j;
        if (data[i] != 0x05)
            continue;
        cursor = i + 1;
        for (j = 0; j < 4 && cursor < bytes; ++j) {
            unsigned char value = data[cursor++];
            length = (length << 7) | (value & 0x7f);
            if ((value & 0x80) == 0)
                break;
        }
        if (j == 4 || cursor + length > bytes || length < 2 || length > 64)
            continue;
        /* NX240 accepts AAC-LC only (Audio Object Type 2). */
        if ((data[cursor] >> 3) != 2)
            continue;
        track->asc_data = box->payload + cursor;
        track->asc_size = (uint32_t)length;
        return NX_MP4_OK;
    }
    return NX_MP4_UNSUPPORTED;
}

static enum nx_mp4_status parse_sample_entry_children(
    const struct nx_reader *reader, uint64_t start, uint64_t end,
    struct nx_mp4_track *track)
{
    uint64_t offset = start;
    uint32_t count = 0;
    while (offset < end) {
        struct nx_box child;
        enum nx_mp4_status status;
        if (++count > NX_MAX_BOXES_PER_LEVEL)
            return NX_MP4_MALFORMED;
        status = read_box(reader, offset, end, &child);
        if (status != NX_MP4_OK)
            return status;
        if (type_is(child.type, "avcC")) {
            status = parse_avcc(reader, &child, track);
            if (status != NX_MP4_OK)
                return status;
        } else if (type_is(child.type, "esds")) {
            status = parse_esds(reader, &child, track);
            if (status != NX_MP4_OK)
                return status;
        }
        offset = child.end;
    }
    return NX_MP4_OK;
}

static enum nx_mp4_status parse_stsd(const struct nx_reader *reader,
                                     const struct nx_box *box,
                                     struct nx_mp4_track *track)
{
    unsigned char head[8];
    struct nx_box entry;
    enum nx_mp4_status status;
    uint32_t entry_count;

    if (box->end - box->payload < sizeof(head) ||
        read_data(reader, box->payload, head, sizeof(head)) != 0)
        return NX_MP4_MALFORMED;
    entry_count = read_be32(head + 4);
    if (entry_count == 0)
        return NX_MP4_MALFORMED;

    status = read_box(reader, box->payload + 8, box->end, &entry);
    if (status != NX_MP4_OK)
        return status;
    memcpy(track->codec, entry.type, 4);
    track->codec[4] = '\0';

    if (type_is(entry.type, "avc1")) {
        unsigned char visual[28];
        if (entry.end - entry.payload < 78 ||
            read_data(reader, entry.payload, visual, sizeof(visual)) != 0)
            return NX_MP4_MALFORMED;
        track->type = NX_TRACK_VIDEO;
        track->width = read_be16(visual + 24);
        track->height = read_be16(visual + 26);
        return parse_sample_entry_children(reader, entry.payload + 78,
                                           entry.end, track);
    }

    if (type_is(entry.type, "mp4a")) {
        unsigned char audio[28];
        if (entry.end - entry.payload < sizeof(audio) ||
            read_data(reader, entry.payload, audio, sizeof(audio)) != 0)
            return NX_MP4_MALFORMED;
        track->type = NX_TRACK_AUDIO;
        track->channels = read_be16(audio + 16);
        track->sample_rate = read_be32(audio + 24) >> 16;
        return parse_sample_entry_children(reader, entry.payload + 28,
                                           entry.end, track);
    }

    return NX_MP4_OK;
}

static enum nx_mp4_status parse_counted_table(
    const struct nx_reader *reader, const struct nx_box *box,
    uint32_t entry_size, uint32_t *count, uint64_t *entries)
{
    uint32_t table_count;
    if (box->end - box->payload < 8 ||
        read_u32(reader, box->payload + 4, &table_count) != 0)
        return NX_MP4_MALFORMED;
    *entries = box->payload + 8;
    if (table_count > (box->end - *entries) / entry_size)
        return NX_MP4_MALFORMED;
    *count = table_count;
    return NX_MP4_OK;
}

static enum nx_mp4_status parse_stsz(const struct nx_reader *reader,
                                     const struct nx_box *box,
                                     struct nx_mp4_track *track)
{
    unsigned char data[12];
    if (box->end - box->payload < sizeof(data) ||
        read_data(reader, box->payload, data, sizeof(data)) != 0)
        return NX_MP4_MALFORMED;
    track->constant_sample_size = read_be32(data + 4);
    track->sample_count = read_be32(data + 8);
    track->stsz_entries = box->payload + 12;
    if (track->constant_sample_size == 0 &&
        track->sample_count > (box->end - track->stsz_entries) / 4)
        return NX_MP4_MALFORMED;
    return NX_MP4_OK;
}

static enum nx_mp4_status parse_stbl(const struct nx_reader *reader,
                                     const struct nx_box *stbl,
                                     struct nx_mp4_track *track)
{
    uint64_t offset = stbl->payload;
    uint32_t count = 0;
    enum nx_mp4_status status;

    while (offset < stbl->end) {
        struct nx_box box;
        if (++count > NX_MAX_BOXES_PER_LEVEL)
            return NX_MP4_MALFORMED;
        status = read_box(reader, offset, stbl->end, &box);
        if (status != NX_MP4_OK)
            return status;

        if (type_is(box.type, "stsd"))
            status = parse_stsd(reader, &box, track);
        else if (type_is(box.type, "stts"))
            status = parse_counted_table(reader, &box, 8,
                                         &track->stts_entry_count,
                                         &track->stts_entries);
        else if (type_is(box.type, "stsc"))
            status = parse_counted_table(reader, &box, 12,
                                         &track->stsc_entry_count,
                                         &track->stsc_entries);
        else if (type_is(box.type, "stsz"))
            status = parse_stsz(reader, &box, track);
        else if (type_is(box.type, "stco")) {
            status = parse_counted_table(reader, &box, 4,
                                         &track->chunk_count,
                                         &track->chunk_entries);
            track->chunk_offsets_64 = 0;
        } else if (type_is(box.type, "co64")) {
            status = parse_counted_table(reader, &box, 8,
                                         &track->chunk_count,
                                         &track->chunk_entries);
            track->chunk_offsets_64 = 1;
        } else if (type_is(box.type, "stss"))
            status = parse_counted_table(reader, &box, 4,
                                         &track->sync_sample_count,
                                         &track->stss_entries);
        else
            status = NX_MP4_OK;

        if (status != NX_MP4_OK)
            return status;
        offset = box.end;
    }

    return NX_MP4_OK;
}

static enum nx_mp4_status parse_minf(const struct nx_reader *reader,
                                     const struct nx_box *minf,
                                     struct nx_mp4_track *track)
{
    uint64_t offset = minf->payload;
    uint32_t count = 0;
    while (offset < minf->end) {
        struct nx_box box;
        enum nx_mp4_status status;
        if (++count > NX_MAX_BOXES_PER_LEVEL)
            return NX_MP4_MALFORMED;
        status = read_box(reader, offset, minf->end, &box);
        if (status != NX_MP4_OK)
            return status;
        if (type_is(box.type, "stbl"))
            return parse_stbl(reader, &box, track);
        offset = box.end;
    }
    return NX_MP4_MALFORMED;
}

static enum nx_mp4_status parse_mdia(const struct nx_reader *reader,
                                     const struct nx_box *mdia,
                                     struct nx_mp4_track *track)
{
    struct nx_box minf;
    uint64_t offset = mdia->payload;
    uint32_t count = 0;
    int has_minf = 0;
    enum nx_mp4_status status = NX_MP4_OK;

    while (offset < mdia->end) {
        struct nx_box box;
        if (++count > NX_MAX_BOXES_PER_LEVEL)
            return NX_MP4_MALFORMED;
        status = read_box(reader, offset, mdia->end, &box);
        if (status != NX_MP4_OK)
            return status;
        if (type_is(box.type, "mdhd"))
            status = parse_mdhd(reader, &box, track);
        else if (type_is(box.type, "hdlr"))
            status = parse_hdlr(reader, &box, track);
        else if (type_is(box.type, "minf")) {
            minf = box;
            has_minf = 1;
        }
        if (status != NX_MP4_OK)
            return status;
        offset = box.end;
    }

    return has_minf ? parse_minf(reader, &minf, track) : NX_MP4_MALFORMED;
}

static enum nx_mp4_status parse_trak(const struct nx_reader *reader,
                                     const struct nx_box *trak,
                                     struct nx_mp4_track *track)
{
    uint64_t offset = trak->payload;
    uint32_t count = 0;
    memset(track, 0, sizeof(*track));

    while (offset < trak->end) {
        struct nx_box box;
        enum nx_mp4_status status;
        if (++count > NX_MAX_BOXES_PER_LEVEL)
            return NX_MP4_MALFORMED;
        status = read_box(reader, offset, trak->end, &box);
        if (status != NX_MP4_OK)
            return status;
        if (type_is(box.type, "mdia")) {
            status = parse_mdia(reader, &box, track);
            if (status != NX_MP4_OK)
                return status;
        }
        offset = box.end;
    }

    track->present = track->type != NX_TRACK_UNKNOWN;
    track->complete = track->present && track->codec[0] != '\0' &&
        track->timescale != 0 && track->sample_count != 0 &&
        track->stts_entry_count != 0 && track->stsc_entry_count != 0 &&
        track->chunk_count != 0;
    return NX_MP4_OK;
}

enum nx_mp4_status nx_mp4_parse(const struct nx_reader *reader,
                                struct nx_mp4_movie *movie)
{
    uint64_t offset;
    uint64_t end;
    uint32_t count = 0;
    enum nx_mp4_status status;

    if (movie == NULL)
        return NX_MP4_IO_ERROR;
    memset(movie, 0, sizeof(*movie));
    status = nx_mp4_probe(reader, &movie->file);
    if (status != NX_MP4_OK)
        return status;

    offset = movie->file.moov_offset + movie->file.moov_header_size;
    end = movie->file.moov_offset + movie->file.moov_size;
    while (offset < end) {
        struct nx_box box;
        if (++count > NX_MAX_BOXES_PER_LEVEL)
            return NX_MP4_MALFORMED;
        status = read_box(reader, offset, end, &box);
        if (status != NX_MP4_OK)
            return status;
        if (type_is(box.type, "trak")) {
            struct nx_mp4_track track;
            status = parse_trak(reader, &box, &track);
            if (status != NX_MP4_OK)
                return status;
            if (track.type == NX_TRACK_VIDEO && !movie->video.present)
                movie->video = track;
            else if (track.type == NX_TRACK_AUDIO && !movie->audio.present)
                movie->audio = track;
        }
        offset = box.end;
    }

    if (!movie->video.present)
        return NX_MP4_MISSING_VIDEO;
    if (!movie->video.complete || memcmp(movie->video.codec, "avc1", 4) != 0 ||
        movie->video.nal_length_size == 0)
        return NX_MP4_UNSUPPORTED;
    if (movie->audio.present &&
        (!movie->audio.complete || memcmp(movie->audio.codec, "mp4a", 4) != 0 ||
         movie->audio.asc_size == 0 || movie->audio.channels == 0 ||
         movie->audio.channels > 2))
        return NX_MP4_UNSUPPORTED;
    return NX_MP4_OK;
}

enum nx_profile_status nx_mp4_validate_nx240(
    const struct nx_mp4_movie *movie)
{
    const struct nx_mp4_track *video;
    const struct nx_mp4_track *audio;

    if (movie == NULL)
        return NX_PROFILE_VIDEO_CODEC;
    video = &movie->video;
    audio = &movie->audio;
    if (!audio->present)
        return NX_PROFILE_MISSING_AUDIO;
    if (video->width == 0 || video->height == 0 ||
        video->width > 320 || video->height > 240)
        return NX_PROFILE_VIDEO_SIZE;
    if (video->avc_profile != 66 || video->avc_level > 13)
        return NX_PROFILE_VIDEO_CODEC;
    if (video->duration == 0 ||
        (uint64_t)video->sample_count * video->timescale >
            (uint64_t)video->duration * 30u)
        return NX_PROFILE_VIDEO_RATE;
    if ((audio->sample_rate != 44100 && audio->sample_rate != 48000) ||
        audio->channels == 0 || audio->channels > 2)
        return NX_PROFILE_AUDIO_FORMAT;
    return NX_PROFILE_OK;
}

static int load_stts(struct nx_mp4_sample_iter *iter)
{
    unsigned char data[8];
    uint64_t offset;
    if (iter->stts_index >= iter->track->stts_entry_count)
        return -1;
    offset = iter->track->stts_entries + (uint64_t)iter->stts_index * 8;
    if (read_data(iter->reader, offset, data, sizeof(data)) != 0)
        return -1;
    iter->stts_samples_left = read_be32(data);
    iter->stts_delta = read_be32(data + 4);
    return iter->stts_samples_left == 0 || iter->stts_delta == 0 ? -1 : 0;
}

static int load_stsc(struct nx_mp4_sample_iter *iter)
{
    unsigned char current[12];
    uint64_t offset = iter->track->stsc_entries +
                      (uint64_t)iter->stsc_index * 12;
    uint32_t first_chunk;
    if (iter->stsc_index >= iter->track->stsc_entry_count ||
        read_data(iter->reader, offset, current, sizeof(current)) != 0)
        return -1;
    first_chunk = read_be32(current);
    iter->samples_per_chunk = read_be32(current + 4);
    if (first_chunk != iter->chunk_index || iter->samples_per_chunk == 0)
        return -1;

    if (iter->stsc_index + 1 < iter->track->stsc_entry_count) {
        if (read_u32(iter->reader, offset + 12, &iter->next_stsc_chunk) != 0 ||
            iter->next_stsc_chunk <= first_chunk)
            return -1;
    } else {
        iter->next_stsc_chunk = UINT32_MAX;
    }
    return 0;
}

static int load_chunk_offset(struct nx_mp4_sample_iter *iter)
{
    uint64_t entry_offset;
    if (iter->chunk_index == 0 ||
        iter->chunk_index > iter->track->chunk_count)
        return -1;
    if (iter->track->chunk_offsets_64) {
        entry_offset = iter->track->chunk_entries +
                       (uint64_t)(iter->chunk_index - 1) * 8;
        if (read_u64(iter->reader, entry_offset, &iter->sample_offset) != 0)
            return -1;
    } else {
        uint32_t value;
        entry_offset = iter->track->chunk_entries +
                       (uint64_t)(iter->chunk_index - 1) * 4;
        if (read_u32(iter->reader, entry_offset, &value) != 0)
            return -1;
        iter->sample_offset = value;
    }
    return iter->sample_offset > iter->reader->size ? -1 : 0;
}

static int sample_dts_for_number(const struct nx_reader *reader,
                                 const struct nx_mp4_track *track,
                                 uint32_t sample_number, uint64_t *dts,
                                 uint32_t *entry_index,
                                 uint32_t *samples_left,
                                 uint32_t *delta)
{
    uint64_t time = 0;
    uint32_t remaining = sample_number - 1;
    uint32_t i;
    for (i = 0; i < track->stts_entry_count; ++i) {
        unsigned char data[8];
        uint32_t count;
        uint32_t step;
        if (read_data(reader, track->stts_entries + (uint64_t)i * 8,
                      data, sizeof(data)) != 0)
            return -1;
        count = read_be32(data);
        step = read_be32(data + 4);
        if (count == 0 || step == 0)
            return -1;
        if (remaining < count) {
            *dts = time + (uint64_t)remaining * step;
            if (entry_index != NULL)
                *entry_index = i;
            if (samples_left != NULL)
                *samples_left = count - remaining;
            if (delta != NULL)
                *delta = step;
            return 0;
        }
        remaining -= count;
        time += (uint64_t)count * step;
    }
    return -1;
}

enum nx_mp4_status nx_mp4_find_sample_at_or_before(
    const struct nx_reader *reader, const struct nx_mp4_track *track,
    uint64_t target_dts, int sync_only, uint32_t *sample_number,
    uint64_t *sample_dts)
{
    uint64_t time = 0;
    uint32_t base = 1;
    uint32_t candidate = 1;
    uint32_t i;

    if (reader == NULL || track == NULL || sample_number == NULL ||
        sample_dts == NULL || track->sample_count == 0)
        return NX_MP4_MALFORMED;
    for (i = 0; i < track->stts_entry_count; ++i) {
        unsigned char data[8];
        uint32_t count;
        uint32_t delta;
        uint64_t span;
        if (read_data(reader, track->stts_entries + (uint64_t)i * 8,
                      data, sizeof(data)) != 0)
            return NX_MP4_IO_ERROR;
        count = read_be32(data);
        delta = read_be32(data + 4);
        if (count == 0 || delta == 0)
            return NX_MP4_MALFORMED;
        span = (uint64_t)count * delta;
        if (target_dts < time + span) {
            uint64_t offset = target_dts <= time ? 0 :
                              (target_dts - time) / delta;
            if (offset >= count)
                offset = count - 1;
            candidate = base + (uint32_t)offset;
            break;
        }
        time += span;
        base += count;
        candidate = base;
    }
    if (candidate > track->sample_count)
        candidate = track->sample_count;

    if (sync_only && track->sync_sample_count != 0) {
        uint32_t low = 0;
        uint32_t high = track->sync_sample_count;
        while (low < high) {
            uint32_t middle = low + (high - low) / 2;
            uint32_t value;
            if (read_u32(reader, track->stss_entries +
                         (uint64_t)middle * 4, &value) != 0)
                return NX_MP4_IO_ERROR;
            if (value <= candidate)
                low = middle + 1;
            else
                high = middle;
        }
        if (low == 0 || read_u32(reader, track->stss_entries +
                                 (uint64_t)(low - 1) * 4,
                                 &candidate) != 0)
            return low == 0 ? NX_MP4_MALFORMED : NX_MP4_IO_ERROR;
    }
    if (sample_dts_for_number(reader, track, candidate, sample_dts,
                              NULL, NULL, NULL) != 0)
        return NX_MP4_MALFORMED;
    *sample_number = candidate;
    return NX_MP4_OK;
}

enum nx_mp4_status nx_mp4_sample_iter_seek(
    struct nx_mp4_sample_iter *iter, const struct nx_reader *reader,
    const struct nx_mp4_track *track, uint32_t sample_number)
{
    uint32_t target_index;
    uint64_t sample_base = 0;
    uint32_t i;

    if (iter == NULL || reader == NULL || track == NULL || !track->complete ||
        sample_number == 0 || sample_number > track->sample_count)
        return NX_MP4_MALFORMED;
    memset(iter, 0, sizeof(*iter));
    iter->reader = reader;
    iter->track = track;
    target_index = sample_number - 1;
    iter->sample_index = target_index;
    if (sample_dts_for_number(reader, track, sample_number, &iter->dts,
                              &iter->stts_index, &iter->stts_samples_left,
                              &iter->stts_delta) != 0)
        return NX_MP4_MALFORMED;

    for (i = 0; i < track->stsc_entry_count; ++i) {
        unsigned char current[12];
        uint32_t first_chunk;
        uint32_t samples_per_chunk;
        uint32_t next_chunk;
        uint64_t run_samples;
        if (read_data(reader, track->stsc_entries + (uint64_t)i * 12,
                      current, sizeof(current)) != 0)
            return NX_MP4_IO_ERROR;
        first_chunk = read_be32(current);
        samples_per_chunk = read_be32(current + 4);
        if (first_chunk == 0 || samples_per_chunk == 0)
            return NX_MP4_MALFORMED;
        if (i + 1 < track->stsc_entry_count) {
            if (read_u32(reader, track->stsc_entries +
                         (uint64_t)(i + 1) * 12, &next_chunk) != 0)
                return NX_MP4_IO_ERROR;
        } else {
            next_chunk = track->chunk_count + 1;
        }
        if (next_chunk <= first_chunk || next_chunk > track->chunk_count + 1)
            return NX_MP4_MALFORMED;
        run_samples = (uint64_t)(next_chunk - first_chunk) *
                      samples_per_chunk;
        if (target_index < sample_base + run_samples) {
            uint64_t within = target_index - sample_base;
            iter->stsc_index = i;
            iter->samples_per_chunk = samples_per_chunk;
            iter->chunk_index = first_chunk +
                (uint32_t)(within / samples_per_chunk);
            iter->sample_in_chunk = (uint32_t)(within % samples_per_chunk);
            iter->next_stsc_chunk = i + 1 < track->stsc_entry_count ?
                                    next_chunk : UINT32_MAX;
            break;
        }
        sample_base += run_samples;
    }
    if (i == track->stsc_entry_count || load_chunk_offset(iter) != 0)
        return NX_MP4_MALFORMED;
    if (iter->sample_in_chunk != 0) {
        if (track->constant_sample_size != 0) {
            iter->sample_offset += (uint64_t)iter->sample_in_chunk *
                                   track->constant_sample_size;
        } else {
            uint32_t first = target_index - iter->sample_in_chunk;
            uint32_t j;
            for (j = first; j < target_index; ++j) {
                uint32_t size;
                if (read_u32(reader, track->stsz_entries +
                             (uint64_t)j * 4, &size) != 0)
                    return NX_MP4_IO_ERROR;
                iter->sample_offset += size;
            }
        }
    }

    if (track->sync_sample_count != 0) {
        uint32_t low = 0;
        uint32_t high = track->sync_sample_count;
        while (low < high) {
            uint32_t middle = low + (high - low) / 2;
            uint32_t value;
            if (read_u32(reader, track->stss_entries +
                         (uint64_t)middle * 4, &value) != 0)
                return NX_MP4_IO_ERROR;
            if (value < sample_number)
                low = middle + 1;
            else
                high = middle;
        }
        iter->sync_index = low;
        if (low < track->sync_sample_count &&
            read_u32(reader, track->stss_entries + (uint64_t)low * 4,
                     &iter->next_sync_sample) != 0)
            return NX_MP4_IO_ERROR;
    }
    return NX_MP4_OK;
}

enum nx_mp4_status nx_mp4_sample_iter_init(
    struct nx_mp4_sample_iter *iter, const struct nx_reader *reader,
    const struct nx_mp4_track *track)
{
    return nx_mp4_sample_iter_seek(iter, reader, track, 1);
}

int nx_mp4_sample_iter_next(struct nx_mp4_sample_iter *iter,
                            struct nx_mp4_sample *sample)
{
    uint32_t size;
    uint32_t sample_number;

    if (iter == NULL || sample == NULL || iter->track == NULL)
        return -1;
    if (iter->sample_index >= iter->track->sample_count)
        return 0;

    if (iter->track->constant_sample_size != 0) {
        size = iter->track->constant_sample_size;
    } else if (read_u32(iter->reader,
                        iter->track->stsz_entries +
                        (uint64_t)iter->sample_index * 4, &size) != 0) {
        return -1;
    }

    if (iter->sample_offset > iter->reader->size ||
        size > iter->reader->size - iter->sample_offset)
        return -1;

    sample_number = iter->sample_index + 1;
    sample->offset = iter->sample_offset;
    sample->size = size;
    sample->dts = iter->dts;
    sample->duration = iter->stts_delta;
    sample->number = sample_number;
    sample->is_sync = iter->track->sync_sample_count == 0 ||
                      sample_number == iter->next_sync_sample;

    if (iter->track->sync_sample_count != 0 && sample->is_sync) {
        iter->sync_index++;
        if (iter->sync_index < iter->track->sync_sample_count) {
            if (read_u32(iter->reader,
                         iter->track->stss_entries +
                         (uint64_t)iter->sync_index * 4,
                         &iter->next_sync_sample) != 0)
                return -1;
        } else {
            iter->next_sync_sample = 0;
        }
    }

    iter->sample_offset += size;
    iter->sample_index++;
    iter->sample_in_chunk++;
    iter->dts += iter->stts_delta;

    if (--iter->stts_samples_left == 0 &&
        iter->sample_index < iter->track->sample_count) {
        iter->stts_index++;
        if (load_stts(iter) != 0)
            return -1;
    }

    if (iter->sample_in_chunk == iter->samples_per_chunk &&
        iter->sample_index < iter->track->sample_count) {
        iter->chunk_index++;
        iter->sample_in_chunk = 0;
        if (iter->chunk_index == iter->next_stsc_chunk) {
            iter->stsc_index++;
            if (load_stsc(iter) != 0)
                return -1;
        }
        if (load_chunk_offset(iter) != 0)
            return -1;
    }

    return 1;
}

const char *nx_mp4_status_string(enum nx_mp4_status status)
{
    switch (status) {
    case NX_MP4_OK: return "NX240 structure OK";
    case NX_MP4_IO_ERROR: return "File read error";
    case NX_MP4_NOT_ISOBMFF: return "Not an MP4 file";
    case NX_MP4_MALFORMED: return "Malformed MP4 boxes";
    case NX_MP4_MISSING_MOOV: return "Missing moov box";
    case NX_MP4_MISSING_MDAT: return "Missing mdat box";
    case NX_MP4_UNSUPPORTED: return "Unsupported MP4 layout";
    case NX_MP4_MISSING_VIDEO: return "No video track";
    default: return "Unknown MP4 error";
    }
}
