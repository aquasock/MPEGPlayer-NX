/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "nx_h264.h"
#include "nx_mp4.h"

struct host_file {
    FILE *stream;
};

static int host_read_at(void *context, uint64_t offset,
                        void *destination, size_t bytes)
{
    struct host_file *file = context;
    if (offset > (uint64_t)LONG_MAX ||
        fseek(file->stream, (long)offset, SEEK_SET) != 0)
        return -1;
    return fread(destination, 1, bytes, file->stream) == bytes ? 0 : -1;
}

static void test_stream(const char *path, uint32_t expected_checksum,
                        unsigned expected_frames)
{
    struct host_file file;
    struct nx_reader reader;
    struct nx_mp4_movie movie;
    struct nx_h264_decoder decoder;
    struct nx_mp4_sample_iter direct_iter;
    struct nx_mp4_sample_iter sequential_iter;
    struct nx_mp4_sample direct_sample;
    struct nx_mp4_sample sequential_sample;
    enum nx_h264_status video_status;
    unsigned frames = 1;
    uint32_t checksum;
    long size;

    file.stream = fopen(path, "rb");
    assert(file.stream != NULL);
    assert(fseek(file.stream, 0, SEEK_END) == 0);
    size = ftell(file.stream);
    assert(size > 0);

    reader.context = &file;
    reader.read_at = host_read_at;
    reader.size = (uint64_t)size;

    assert(nx_mp4_parse(&reader, &movie) == NX_MP4_OK);
    assert(movie.video.avcc_size > 7);
    assert(movie.audio.asc_size >= 2);

    {
        uint32_t target = movie.video.sample_count / 2 + 1;
        uint32_t i;
        uint32_t sync_number;
        uint64_t sync_dts;
        assert(nx_mp4_sample_iter_init(&sequential_iter, &reader,
                                       &movie.video) == NX_MP4_OK);
        for (i = 0; i < target; ++i)
            assert(nx_mp4_sample_iter_next(&sequential_iter,
                                           &sequential_sample) == 1);
        assert(nx_mp4_sample_iter_seek(&direct_iter, &reader, &movie.video,
                                       target) == NX_MP4_OK);
        assert(nx_mp4_sample_iter_next(&direct_iter, &direct_sample) == 1);
        assert(direct_sample.number == sequential_sample.number);
        assert(direct_sample.offset == sequential_sample.offset);
        assert(direct_sample.size == sequential_sample.size);
        assert(direct_sample.dts == sequential_sample.dts);
        assert(nx_mp4_find_sample_at_or_before(&reader, &movie.video,
                   movie.video.duration / 2, 1, &sync_number,
                   &sync_dts) == NX_MP4_OK);
        assert(sync_number <= target);
        assert(sync_dts <= movie.video.duration / 2);
    }
    {
        uint32_t target = movie.audio.sample_count / 2 + 1;
        uint32_t i;
        assert(nx_mp4_sample_iter_init(&sequential_iter, &reader,
                                       &movie.audio) == NX_MP4_OK);
        for (i = 0; i < target; ++i)
            assert(nx_mp4_sample_iter_next(&sequential_iter,
                                           &sequential_sample) == 1);
        assert(nx_mp4_sample_iter_seek(&direct_iter, &reader, &movie.audio,
                                       target) == NX_MP4_OK);
        assert(nx_mp4_sample_iter_next(&direct_iter, &direct_sample) == 1);
        assert(direct_sample.number == sequential_sample.number);
        assert(direct_sample.offset == sequential_sample.offset);
        assert(direct_sample.size == sequential_sample.size);
        assert(direct_sample.dts == sequential_sample.dts);
    }
    assert(nx_h264_memory_init(NULL, 0) == 0);
    assert(nx_h264_decoder_init(&decoder) == NX_H264_OK);
    assert(nx_h264_decode_first_picture(&decoder, &reader, &movie.video) ==
           NX_H264_OK);
    assert(decoder.width == 320);
    assert(decoder.height == 240);
    assert(decoder.picture != NULL);
    checksum = nx_h264_picture_checksum(&decoder);
    printf("%s: first-frame FNV-1a %08x\n", path, checksum);
    if (expected_checksum != 0)
        assert(checksum == expected_checksum);
    assert(decoder.error_mbs == 0);

    while ((video_status = nx_h264_decode_next_picture(&decoder)) ==
           NX_H264_OK) {
        assert(decoder.picture != NULL);
        assert(decoder.error_mbs == 0);
        frames++;
    }
    assert(video_status == NX_H264_END_OF_STREAM);
    assert(frames == expected_frames);
    printf("%s: decoded frames %u\n", path, frames);

    nx_h264_decoder_destroy(&decoder);

    {
        uint32_t sync_number;
        uint64_t sync_dts;
        unsigned seek_frames = 0;
        assert(nx_mp4_find_sample_at_or_before(&reader, &movie.video,
                   movie.video.duration / 2, 1, &sync_number,
                   &sync_dts) == NX_MP4_OK);
        assert(nx_h264_decoder_init(&decoder) == NX_H264_OK);
        assert(nx_h264_stream_start_at(&decoder, &reader, &movie.video,
                                       sync_number) == NX_H264_OK);
        while ((video_status = nx_h264_decode_next_picture(&decoder)) ==
               NX_H264_OK) {
            assert(decoder.error_mbs == 0);
            seek_frames++;
        }
        assert(video_status == NX_H264_END_OF_STREAM);
        assert(seek_frames == movie.video.sample_count - sync_number + 1);
        nx_h264_decoder_destroy(&decoder);
    }
    nx_h264_memory_destroy();
    fclose(file.stream);
}

int main(void)
{
    test_stream("dist/erosqnative/nx240-test.nxv", 0x24cd5247u, 192);
    test_stream("dist/erosqnative/nx240-stress.nxv", 0x94ba8733u, 480);
    test_stream("dist/erosqnative/nx240-av-sync.nxv", 0xb9e247eeu, 1440);
    puts("H.264 stream tests passed");
    return 0;
}
