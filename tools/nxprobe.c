/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "nx_mp4.h"

#include <limits.h>
#include <stdio.h>

static int file_read_at(void *context, uint64_t offset,
                        void *destination, size_t bytes)
{
    FILE *file = context;
    if (offset > (uint64_t)LONG_MAX || fseek(file, (long)offset, SEEK_SET) != 0)
        return -1;
    return fread(destination, 1, bytes, file) == bytes ? 0 : -1;
}

static int inspect_track(const struct nx_reader *reader,
                         const struct nx_mp4_track *track, const char *name)
{
    struct nx_mp4_sample_iter iter;
    struct nx_mp4_sample sample;
    uint32_t count = 0;
    uint32_t sync_count = 0;
    uint64_t end_dts = 0;
    int result;

    if (!track->present) {
        printf("%s: absent\n", name);
        return 0;
    }

    printf("%s: codec=%s timescale=%u duration=%llu samples=%u chunks=%u",
           name, track->codec, track->timescale,
           (unsigned long long)track->duration,
           track->sample_count, track->chunk_count);
    if (track->type == NX_TRACK_VIDEO) {
        printf(" size=%ux%u avc=%u/%u nal=%u",
               track->width, track->height, track->avc_profile,
               track->avc_level, track->nal_length_size);
    } else {
        printf(" rate=%u channels=%u", track->sample_rate, track->channels);
    }
    putchar('\n');

    if (nx_mp4_sample_iter_init(&iter, reader, track) != NX_MP4_OK) {
        fprintf(stderr, "%s iterator initialization failed\n", name);
        return -1;
    }
    while ((result = nx_mp4_sample_iter_next(&iter, &sample)) == 1) {
        count++;
        sync_count += (uint32_t)sample.is_sync;
        end_dts = sample.dts + sample.duration;
    }
    if (result < 0 || count != track->sample_count) {
        fprintf(stderr, "%s iterator failed at sample %u\n", name, count + 1);
        return -1;
    }
    printf("%s index: samples=%u sync=%u end_dts=%llu\n", name, count,
           sync_count, (unsigned long long)end_dts);
    return 0;
}

int main(int argc, char **argv)
{
    struct nx_mp4_movie movie;
    struct nx_reader reader;
    enum nx_mp4_status status;
    FILE *file;
    long size;

    if (argc != 2) {
        fprintf(stderr, "usage: %s FILE.mp4\n", argv[0]);
        return 2;
    }
    file = fopen(argv[1], "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0 ||
        (size = ftell(file)) < 0) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        if (file != NULL)
            fclose(file);
        return 1;
    }

    reader.context = file;
    reader.read_at = file_read_at;
    reader.size = (uint64_t)size;
    status = nx_mp4_parse(&reader, &movie);
    printf("%s: %s; brand=%s; fast_start=%s\n", argv[1],
           nx_mp4_status_string(status), movie.file.major_brand,
           movie.file.fast_start ? "yes" : "no");
    if (status != NX_MP4_OK ||
        inspect_track(&reader, &movie.video, "video") != 0 ||
        inspect_track(&reader, &movie.audio, "audio") != 0) {
        fclose(file);
        return 1;
    }
    fclose(file);
    return 0;
}
