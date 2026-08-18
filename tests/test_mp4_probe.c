/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "nx_mp4.h"

#include <stdio.h>
#include <string.h>

struct memory_file {
    const unsigned char *data;
    size_t size;
};

static int memory_read_at(void *context, uint64_t offset,
                          void *destination, size_t bytes)
{
    const struct memory_file *file = context;
    if (offset > file->size || bytes > file->size - (size_t)offset)
        return -1;
    memcpy(destination, file->data + (size_t)offset, bytes);
    return 0;
}

static int failures;

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", \
                __FILE__, __LINE__, #expression); \
        failures++; \
    } \
} while (0)

static enum nx_mp4_status probe(const unsigned char *data, size_t size,
                                struct nx_mp4_info *info)
{
    struct memory_file file = { data, size };
    struct nx_reader reader = { &file, memory_read_at, size };
    return nx_mp4_probe(&reader, info);
}

static void write_be32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

static void test_fast_start(void)
{
    static const unsigned char file[] = {
        0,0,0,16, 'f','t','y','p', 'i','s','o','m', 0,0,2,0,
        0,0,0,8,  'm','o','o','v',
        0,0,0,8,  'm','d','a','t'
    };
    struct nx_mp4_info info;
    CHECK(probe(file, sizeof(file), &info) == NX_MP4_OK);
    CHECK(strcmp(info.major_brand, "isom") == 0);
    CHECK(info.top_level_box_count == 3);
    CHECK(info.fast_start == 1);
}

static void test_slow_start(void)
{
    static const unsigned char file[] = {
        0,0,0,16, 'f','t','y','p', 'm','p','4','2', 0,0,0,0,
        0,0,0,8,  'm','d','a','t',
        0,0,0,8,  'm','o','o','v'
    };
    struct nx_mp4_info info;
    CHECK(probe(file, sizeof(file), &info) == NX_MP4_OK);
    CHECK(info.fast_start == 0);
}

static void test_extended_size(void)
{
    static const unsigned char file[] = {
        0,0,0,16, 'f','t','y','p', 'i','s','o','m', 0,0,0,0,
        0,0,0,1,  'm','o','o','v', 0,0,0,0, 0,0,0,16,
        0,0,0,8,  'm','d','a','t'
    };
    struct nx_mp4_info info;
    CHECK(probe(file, sizeof(file), &info) == NX_MP4_OK);
    CHECK(info.top_level_box_count == 3);
}

static void test_rejections(void)
{
    static const unsigned char not_mp4[] = {
        0,0,0,8, 'f','r','e','e', 0,0,0,8, 'm','o','o','v',
        0,0,0,8, 'm','d','a','t'
    };
    static const unsigned char malformed[] = {
        0,0,0,7, 'f','t','y','p'
    };
    static const unsigned char no_moov[] = {
        0,0,0,16, 'f','t','y','p', 'i','s','o','m', 0,0,0,0,
        0,0,0,8, 'm','d','a','t'
    };
    struct nx_mp4_info info;
    CHECK(probe(not_mp4, sizeof(not_mp4), &info) == NX_MP4_NOT_ISOBMFF);
    CHECK(probe(malformed, sizeof(malformed), &info) == NX_MP4_MALFORMED);
    CHECK(probe(no_moov, sizeof(no_moov), &info) == NX_MP4_MISSING_MOOV);
}

static void test_sample_iterator(void)
{
    unsigned char file[128] = { 0 };
    struct memory_file memory = { file, sizeof(file) };
    struct nx_reader reader = { &memory, memory_read_at, sizeof(file) };
    struct nx_mp4_track track;
    struct nx_mp4_sample_iter iter;
    struct nx_mp4_sample sample;
    static const uint64_t expected_offsets[] = { 96, 99, 110, 120 };
    static const uint32_t expected_sizes[] = { 3, 4, 5, 6 };
    uint32_t i;

    memset(&track, 0, sizeof(track));
    write_be32(file + 0, 4);       /* stts sample count */
    write_be32(file + 4, 10);      /* stts sample delta */
    write_be32(file + 8, 1);       /* stsc first chunk */
    write_be32(file + 12, 2);      /* two samples in chunk 1 */
    write_be32(file + 16, 1);      /* sample description */
    write_be32(file + 20, 2);      /* stsc changes at chunk 2 */
    write_be32(file + 24, 1);      /* one sample per later chunk */
    write_be32(file + 28, 1);
    for (i = 0; i < 4; i++)
        write_be32(file + 32 + i * 4, expected_sizes[i]);
    write_be32(file + 48, 96);
    write_be32(file + 52, 110);
    write_be32(file + 56, 120);
    write_be32(file + 60, 1);      /* sync samples 1 and 4 */
    write_be32(file + 64, 4);

    track.present = 1;
    track.complete = 1;
    track.sample_count = 4;
    track.stts_entry_count = 1;
    track.stts_entries = 0;
    track.stsc_entry_count = 2;
    track.stsc_entries = 8;
    track.stsz_entries = 32;
    track.chunk_count = 3;
    track.chunk_entries = 48;
    track.sync_sample_count = 2;
    track.stss_entries = 60;

    CHECK(nx_mp4_sample_iter_init(&iter, &reader, &track) == NX_MP4_OK);
    for (i = 0; i < 4; i++) {
        CHECK(nx_mp4_sample_iter_next(&iter, &sample) == 1);
        CHECK(sample.number == i + 1);
        CHECK(sample.offset == expected_offsets[i]);
        CHECK(sample.size == expected_sizes[i]);
        CHECK(sample.dts == (uint64_t)i * 10);
        CHECK(sample.duration == 10);
        CHECK(sample.is_sync == (i == 0 || i == 3));
    }
    CHECK(nx_mp4_sample_iter_next(&iter, &sample) == 0);
}

static void test_nx240_profile(void)
{
    struct nx_mp4_movie movie;

    memset(&movie, 0, sizeof(movie));
    movie.video.width = 320;
    movie.video.height = 240;
    movie.video.avc_profile = 66;
    movie.video.avc_level = 13;
    movie.video.timescale = 1000;
    movie.video.duration = 10000;
    movie.video.sample_count = 300;
    movie.audio.present = 1;
    movie.audio.sample_rate = 44100;
    movie.audio.channels = 2;
    CHECK(nx_mp4_validate_nx240(&movie) == NX_PROFILE_OK);

    movie.audio.present = 0;
    CHECK(nx_mp4_validate_nx240(&movie) == NX_PROFILE_MISSING_AUDIO);
    movie.audio.present = 1;
    movie.video.width = 321;
    CHECK(nx_mp4_validate_nx240(&movie) == NX_PROFILE_VIDEO_SIZE);
    movie.video.width = 320;
    movie.video.avc_profile = 77;
    CHECK(nx_mp4_validate_nx240(&movie) == NX_PROFILE_VIDEO_CODEC);
    movie.video.avc_profile = 66;
    movie.video.sample_count = 301;
    CHECK(nx_mp4_validate_nx240(&movie) == NX_PROFILE_VIDEO_RATE);
    movie.video.sample_count = 300;
    movie.audio.sample_rate = 32000;
    CHECK(nx_mp4_validate_nx240(&movie) == NX_PROFILE_AUDIO_FORMAT);
}

int main(void)
{
    test_fast_start();
    test_slow_start();
    test_extended_size();
    test_rejections();
    test_sample_iterator();
    test_nx240_profile();
    if (failures != 0)
        return 1;
    puts("MP4 probe tests passed");
    return 0;
}
