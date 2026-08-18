/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "plugin.h"
#include "lib/helper.h"
#include "nx_aac.h"
#include "nx_h264.h"
#include "nx_mp4.h"

#define NX_AUDIO_PREBUFFER (96u * 1024u)
#define NX_OSD_SECONDS 2

static void draw_message_panel(const char *line1, const char *line2,
                               const char *line3);

struct rockbox_file {
    int fd;
};

static int rockbox_read_at(void *context, uint64_t offset,
                           void *destination, size_t bytes)
{
    struct rockbox_file *file = context;
    if (rb->lseek(file->fd, (off_t)offset, SEEK_SET) < 0)
        return -1;
    return rb->read(file->fd, destination, bytes) == (ssize_t)bytes ? 0 : -1;
}

static void draw_probe(const char *path, enum nx_mp4_status status,
                       const struct nx_mp4_movie *movie)
{
    int y = 2;
    int line_height;

#ifdef HAVE_LCD_COLOR
    rb->lcd_set_backdrop(NULL);
    rb->lcd_set_foreground(LCD_WHITE);
    rb->lcd_set_background(LCD_BLACK);
#endif
    rb->lcd_clear_display();
    rb->lcd_setfont(FONT_UI);
    line_height = rb->font_get(FONT_UI)->height + 3;
    rb->lcd_putsxy(4, y, "MPEGPlayer NX  M4");
    y += line_height;
    rb->lcd_putsxyf(4, y, "%s", rb->strrchr(path, '/') != NULL ?
                    rb->strrchr(path, '/') + 1 : path);
    y += line_height;
    rb->lcd_putsxyf(4, y, "%s", nx_mp4_status_string(status));

    if (status == NX_MP4_OK) {
        unsigned long fps100 = movie->video.duration == 0 ? 0 :
            (unsigned long)((uint64_t)movie->video.sample_count *
            movie->video.timescale * 100 / movie->video.duration);
        y += line_height;
        rb->lcd_putsxyf(4, y, "Video: %s %lux%lu",
                        movie->video.codec,
                        (unsigned long)movie->video.width,
                        (unsigned long)movie->video.height);
        y += line_height;
        rb->lcd_putsxyf(4, y, "AVC: P%u L%u NAL%u",
                        movie->video.avc_profile, movie->video.avc_level,
                        movie->video.nal_length_size);
        y += line_height;
        rb->lcd_putsxyf(4, y, "Frames: %lu @ %lu.%02lu fps",
                        (unsigned long)movie->video.sample_count,
                        fps100 / 100, fps100 % 100);
        y += line_height;
        if (movie->audio.present) {
            rb->lcd_putsxyf(4, y, "Audio: %s %luHz %luch",
                            movie->audio.codec,
                            (unsigned long)movie->audio.sample_rate,
                            (unsigned long)movie->audio.channels);
            y += line_height;
        }
        rb->lcd_putsxyf(4, y, "Fast start: %s; index ready",
                        movie->file.fast_start ? "yes" : "no");
    }

    rb->lcd_update();
}

static void draw_decode_error(const char *path, enum nx_h264_status status,
                              enum nx_aac_status audio_status,
                              size_t available, size_t peak)
{
    int y = 4;
    int line_height;
#ifdef HAVE_LCD_COLOR
    rb->lcd_set_backdrop(NULL);
    rb->lcd_set_foreground(LCD_WHITE);
    rb->lcd_set_background(LCD_BLACK);
#endif
    rb->lcd_clear_display();
    rb->lcd_setfont(FONT_UI);
    line_height = rb->font_get(FONT_UI)->height + 4;
    rb->lcd_putsxy(4, y, "MPEGPlayer NX  M4");
    y += line_height;
    rb->lcd_putsxyf(4, y, "%s", rb->strrchr(path, '/') != NULL ?
                    rb->strrchr(path, '/') + 1 : path);
    y += line_height * 2;
    rb->lcd_putsxyf(4, y, "%s", nx_h264_status_string(status));
    y += line_height;
    if (audio_status != NX_AAC_OK) {
        rb->lcd_putsxyf(4, y, "%s", nx_aac_status_string(audio_status));
        y += line_height;
    }
    rb->lcd_putsxyf(4, y, "Pool: %luK; peak: %luK",
                    (unsigned long)(available / 1024),
                    (unsigned long)(peak / 1024));
    rb->lcd_update();
}

static void draw_profile_error(const char *path,
                               enum nx_profile_status status)
{
    const char *reason;
    const char *requirement;

    switch (status) {
    case NX_PROFILE_MISSING_AUDIO:
        reason = "No AAC audio track";
        requirement = "AAC-LC mono/stereo required";
        break;
    case NX_PROFILE_VIDEO_SIZE:
        reason = "Video dimensions too large";
        requirement = "Maximum 320x240";
        break;
    case NX_PROFILE_VIDEO_CODEC:
        reason = "Unsupported H.264 profile";
        requirement = "Baseline Level 1.3 or lower";
        break;
    case NX_PROFILE_VIDEO_RATE:
        reason = "Video frame rate too high";
        requirement = "Maximum 30 fps";
        break;
    case NX_PROFILE_AUDIO_FORMAT:
        reason = "Unsupported AAC format";
        requirement = "44.1/48 kHz, mono/stereo";
        break;
    default:
        reason = "Unsupported NX240 file";
        requirement = "Check the encode profile";
        break;
    }
    draw_message_panel(rb->strrchr(path, '/') != NULL ?
                       rb->strrchr(path, '/') + 1 : path,
                       reason, requirement);
}

struct playback_stats {
    uint32_t decoded;
    uint32_t displayed;
    uint32_t dropped;
    uint32_t checksum;
    uint64_t decode_us;
    uint32_t worst_decode_us;
};

static uint32_t precision_time(void)
{
#if CONFIG_CPU == X1000 && !defined(SIMULATOR)
    return __ost_read32();
#else
    return (uint32_t)*rb->current_tick;
#endif
}

static uint32_t precision_elapsed_us(uint32_t start)
{
    uint32_t elapsed = precision_time() - start;
#if CONFIG_CPU == X1000 && !defined(SIMULATOR)
    return elapsed / OST_TICKS_PER_US;
#else
    return elapsed * (1000000u / HZ);
#endif
}

static int playback_osd_y(void)
{
    int line_height;
    int panel_height;

    rb->lcd_setfont(FONT_UI);
    line_height = rb->font_get(FONT_UI)->height;
    panel_height = line_height + 11;
    return (LCD_HEIGHT - panel_height) & ~1;
}

static void draw_picture(const struct nx_h264_decoder *decoder,
                         int visible_bottom)
{
    unsigned char *planes[3];
    uint64_t luma_size = (uint64_t)decoder->coded_width *
                         decoder->coded_height;
    int x = (LCD_WIDTH - (int)decoder->width) / 2;
    int y = (LCD_HEIGHT - (int)decoder->height) / 2;
    int height = decoder->height;

    if (visible_bottom < y + height)
        height = visible_bottom - y;
    /* lcd_blit_yuv consumes 4:2:0 data, so the clipped height must be even. */
    height &= ~1;
    if (height <= 0)
        return;

    planes[0] = decoder->picture;
    planes[1] = decoder->picture + luma_size;
    planes[2] = planes[1] + luma_size / 4;

    rb->lcd_blit_yuv(planes, decoder->crop_left, decoder->crop_top,
                     decoder->coded_width, x, y,
                     decoder->width, height);
}

static void draw_playback_osd(uint64_t elapsed_us, uint64_t duration_us)
{
    char text[64];
    int panel_y;
    int bar_width;
    int fill_width;
    int volume = rb->sound_val2phys(SOUND_VOLUME,
                                    rb->global_status->volume);

    if (elapsed_us > duration_us)
        elapsed_us = duration_us;
    rb->lcd_setfont(FONT_UI);
    panel_y = playback_osd_y();
    bar_width = LCD_WIDTH - 8;
    fill_width = duration_us == 0 ? 0 :
        (int)(elapsed_us * bar_width / duration_us);

    rb->lcd_set_foreground(LCD_BLACK);
    rb->lcd_fillrect(0, panel_y, LCD_WIDTH, LCD_HEIGHT - panel_y);
    rb->lcd_set_foreground(LCD_WHITE);
    rb->snprintf(text, sizeof(text), "%lu:%02lu / %lu:%02lu   Vol %d%s",
                 (unsigned long)(elapsed_us / 60000000u),
                 (unsigned long)(elapsed_us / 1000000u % 60),
                 (unsigned long)(duration_us / 60000000u),
                 (unsigned long)(duration_us / 1000000u % 60),
                 volume, rb->sound_unit(SOUND_VOLUME));
    rb->lcd_putsxy(4, panel_y + 1, text);
    rb->lcd_drawrect(4, LCD_HEIGHT - 7, bar_width, 5);
    if (fill_width > 2)
        rb->lcd_fillrect(5, LCD_HEIGHT - 6, fill_width - 2, 3);
}

static void draw_message_panel(const char *line1, const char *line2,
                               const char *line3)
{
    int line_height;
    int panel_height;

    rb->lcd_setfont(FONT_UI);
    line_height = rb->font_get(FONT_UI)->height;
    panel_height = line_height * (line3 == NULL ? 2 : 3) + 4;
    rb->lcd_set_foreground(LCD_BLACK);
    rb->lcd_fillrect(0, 0, LCD_WIDTH, panel_height);
    rb->lcd_set_foreground(LCD_WHITE);
    rb->lcd_putsxy(3, 1, line1);
    rb->lcd_putsxy(3, line_height + 2, line2);
    if (line3 != NULL)
        rb->lcd_putsxy(3, line_height * 2 + 3, line3);
    rb->lcd_update();
}

static void draw_playback_stats(const struct playback_stats *stats,
                                const struct nx_aac_decoder *audio,
                                const struct nx_pcm_output *output,
                                size_t peak)
{
    char line1[48];
    char line2[48];
    char line3[48];
    unsigned long average_us = stats->decoded == 0 ? 0 :
        (unsigned long)(stats->decode_us / stats->decoded);
    unsigned long worst_us = stats->worst_decode_us;

    unsigned long audio_average_us = audio->decoded_frames == 0 ? 0 :
        (unsigned long)(audio->decode_us / audio->decoded_frames);

    rb->snprintf(line1, sizeof(line1), "NX M4 A/V: %lu/%lu drop %lu",
                 (unsigned long)stats->displayed,
                 (unsigned long)stats->decoded,
                 (unsigned long)stats->dropped);
    rb->snprintf(line2, sizeof(line2), "avg %lu.%lums max %lu.%lums mem %luK",
                 average_us / 1000, (average_us % 1000) / 100,
                 worst_us / 1000, (worst_us % 1000) / 100,
                 (unsigned long)(peak / 1024));
    rb->snprintf(line3, sizeof(line3), "AAC %lu.%lums max %lu.%lums under %lu",
                 audio_average_us / 1000, (audio_average_us % 1000) / 100,
                 (unsigned long)audio->worst_decode_us / 1000,
                 ((unsigned long)audio->worst_decode_us % 1000) / 100,
                 (unsigned long)output->underruns);
    draw_message_panel(line1, line2, line3);
}

/* 0 = continue, 1 = user exit, 2 = USB, 3 = refresh OSD,
 * 4 = seek backward, 5 = seek forward. */
static int process_button(int button, struct nx_pcm_output *output)
{
    if (button == BUTTON_NONE)
        return 0;
    if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
        return 2;

#ifdef BUTTON_VOL_UP
    if ((button & (BUTTON_VOL_UP | BUTTON_REL)) == BUTTON_VOL_UP) {
        int volume = rb->global_status->volume;
        if (volume < rb->sound_max(SOUND_VOLUME)) {
            volume++;
            rb->sound_set(SOUND_VOLUME, volume);
            rb->global_status->volume = volume;
        }
        return 3;
    }
#endif
#ifdef BUTTON_VOL_DOWN
    if ((button & (BUTTON_VOL_DOWN | BUTTON_REL)) == BUTTON_VOL_DOWN) {
        int volume = rb->global_status->volume;
        if (volume > rb->sound_min(SOUND_VOLUME)) {
            volume--;
            rb->sound_set(SOUND_VOLUME, volume);
            rb->global_status->volume = volume;
        }
        return 3;
    }
#endif
#ifdef BUTTON_MENU
    if ((button & (BUTTON_MENU | BUTTON_REL | BUTTON_REPEAT)) == BUTTON_MENU)
        return 3;
#endif
#ifdef BUTTON_PREV
    if ((button & (BUTTON_PREV | BUTTON_REL)) == BUTTON_PREV)
        return 4;
#endif
#ifdef BUTTON_NEXT
    if ((button & (BUTTON_NEXT | BUTTON_REL)) == BUTTON_NEXT)
        return 5;
#endif

#ifdef BUTTON_PLAY
    if ((button & (BUTTON_PLAY | BUTTON_REL | BUTTON_REPEAT)) == BUTTON_PLAY) {
        nx_pcm_output_pause(output, 0);
        draw_message_panel("MPEGPlayer NX paused",
                           "Play: resume  Power: exit", NULL);
        while (1) {
            button = rb->button_get(true);
            if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
                return 2;
#ifdef BUTTON_POWER
            if ((button & BUTTON_POWER) != 0)
                return 1;
#endif
            if ((button & (BUTTON_PLAY | BUTTON_REL | BUTTON_REPEAT)) ==
                    BUTTON_PLAY) {
                nx_pcm_output_pause(output, 1);
                return 0;
            }
        }
    }
#endif

#ifdef BUTTON_POWER
    if ((button & BUTTON_POWER) != 0)
        return 1;
#endif
#ifdef BUTTON_BACK
    if ((button & BUTTON_BACK) != 0)
        return 1;
#endif
    return 0;
}

static enum nx_aac_status fill_audio(struct nx_aac_decoder *decoder,
                                     struct nx_pcm_output *output,
                                     uint32_t target_bytes,
                                     int *audio_ended)
{
    while (!*audio_ended && nx_pcm_output_used(output) < target_bytes) {
        enum nx_aac_status status;
        uint32_t frames;

        status = nx_aac_decode_next(decoder, &frames);
        if (status == NX_AAC_END_OF_STREAM) {
            *audio_ended = 1;
            nx_pcm_output_mark_end(output);
            return NX_AAC_OK;
        }
        if (status != NX_AAC_OK)
            return status;
        if (frames != 0 && nx_pcm_output_write(output, decoder->pcm,
                                                frames) != 0)
            return NX_AAC_NO_MEMORY;
    }
    return NX_AAC_OK;
}

static int restart_av_at(struct nx_h264_decoder *video,
                         struct nx_aac_decoder *audio,
                         struct nx_pcm_output *output,
                         const struct nx_reader *reader,
                         const struct nx_mp4_track *video_track,
                         const struct nx_mp4_track *audio_track,
                         uint64_t requested_us, int *audio_ended,
                         enum nx_h264_status *video_status,
                         enum nx_aac_status *audio_status)
{
    uint32_t video_sample;
    uint32_t audio_sample;
    uint64_t video_dts;
    uint64_t audio_dts;
    uint64_t audio_base_us;
    uint64_t requested_video_dts = requested_us * video_track->timescale /
                                   1000000u;

    if (nx_mp4_find_sample_at_or_before(reader, video_track,
            requested_video_dts, 1, &video_sample, &video_dts) != NX_MP4_OK)
        return -1;
    if (nx_mp4_find_sample_at_or_before(reader, audio_track,
            video_dts * audio_track->timescale / video_track->timescale,
            0, &audio_sample, &audio_dts) != NX_MP4_OK)
        return -1;

    nx_pcm_output_reset(output, 0);
    nx_h264_decoder_destroy(video);
    nx_aac_decoder_destroy(audio);

    *video_status = nx_h264_decoder_init(video);
    if (*video_status != NX_H264_OK)
        return -1;
    *video_status = nx_h264_stream_start_at(video, reader, video_track,
                                            video_sample);
    if (*video_status != NX_H264_OK)
        return -1;
    *audio_status = nx_aac_decoder_init(audio, reader, audio_track);
    if (*audio_status != NX_AAC_OK)
        return -1;
    *audio_status = nx_aac_decoder_seek(audio, audio_sample);
    if (*audio_status != NX_AAC_OK)
        return -1;

    audio_base_us = audio_dts * 1000000u / audio_track->timescale;
    nx_pcm_output_reset(output, audio_base_us);
    *audio_ended = 0;
    *audio_status = fill_audio(audio, output, NX_AUDIO_PREBUFFER,
                               audio_ended);
    if (*audio_status != NX_AAC_OK)
        return -1;
    nx_pcm_output_start(output);
    return 0;
}

static int drain_audio(struct nx_aac_decoder *audio,
                       struct nx_pcm_output *output, int *audio_ended,
                       enum nx_aac_status *audio_status,
                       uint64_t duration_us)
{
    while (!nx_pcm_output_finished(output)) {
        int event;
        *audio_status = fill_audio(audio, output, NX_AUDIO_PREBUFFER,
                                   audio_ended);
        if (*audio_status != NX_AAC_OK)
            return 0;
        event = process_button(rb->button_get_w_tmo(1), output);
        if (event == 3) {
            draw_playback_osd(duration_us, duration_us);
            rb->lcd_update();
        } else if (event == 1 || event == 2) {
            return event;
        }
        rb->reset_poweroff_timer();
    }
    return 0;
}

static int play_av(struct nx_h264_decoder *video,
                   struct nx_aac_decoder *audio,
                   struct nx_pcm_output *output,
                   const struct nx_reader *reader,
                   const struct nx_mp4_track *track,
                   const struct nx_mp4_track *audio_track,
                   struct playback_stats *stats,
                   enum nx_h264_status *video_status,
                   enum nx_aac_status *audio_status)
{
    uint64_t clock_denominator;
    uint64_t frame_period_us;
    uint64_t duration_us;
    int audio_ended = 0;
    long osd_until = *rb->current_tick + NX_OSD_SECONDS * HZ;
    uint32_t osd_second = UINT32_MAX;

    rb->memset(stats, 0, sizeof(*stats));
    *video_status = nx_h264_stream_start(video, reader, track);
    if (*video_status != NX_H264_OK)
        return 0;

    clock_denominator = (uint64_t)track->sample_count * track->timescale;
    if (clock_denominator == 0 || track->duration == 0) {
        *video_status = NX_H264_BAD_CONFIG;
        return 0;
    }
    frame_period_us = (uint64_t)track->duration * 1000000u /
                      clock_denominator;
    if (frame_period_us == 0)
        frame_period_us = 1;
    duration_us = (uint64_t)track->duration * 1000000u / track->timescale;

    *audio_status = fill_audio(audio, output, NX_AUDIO_PREBUFFER,
                               &audio_ended);
    if (*audio_status != NX_AAC_OK)
        return 0;

    rb->lcd_set_backdrop(NULL);
    rb->lcd_set_background(LCD_BLACK);
    rb->lcd_clear_display();
    rb->lcd_update();
    nx_pcm_output_start(output);

    while (1) {
        uint32_t decode_start = precision_time();
        uint32_t decode_us;
        uint64_t target_us;
        uint64_t audio_clock_us;
        int event;
        int seek_event = 0;

        *audio_status = fill_audio(audio, output, NX_AUDIO_PREBUFFER,
                                   &audio_ended);
        if (*audio_status != NX_AAC_OK)
            return 0;

        *video_status = nx_h264_decode_next_picture(video);
        decode_us = precision_elapsed_us(decode_start);
        if (*video_status == NX_H264_END_OF_STREAM)
            return drain_audio(audio, output, &audio_ended, audio_status,
                               duration_us);
        if (*video_status != NX_H264_OK)
            return 0;

        stats->decoded++;
        stats->decode_us += decode_us;
        if (decode_us > stats->worst_decode_us)
            stats->worst_decode_us = decode_us;
        if (stats->decoded == 1)
            stats->checksum = nx_h264_picture_checksum(video);

        target_us = video->sample.dts * 1000000u / track->timescale;
        audio_clock_us = nx_pcm_output_clock_us(output);
        while (audio_clock_us < target_us) {
            /* A malformed or badly trimmed file may end audio before video.
             * Stop cleanly at the last audible timestamp instead of waiting
             * forever on an audio clock which can no longer advance. */
            if (audio_ended && nx_pcm_output_finished(output)) {
                *video_status = NX_H264_END_OF_STREAM;
                return 0;
            }
            event = process_button(rb->button_get_w_tmo(1), output);
            if (event == 3) {
                osd_until = *rb->current_tick + NX_OSD_SECONDS * HZ;
                event = 0;
            }
            if (event == 4 || event == 5) {
                seek_event = event;
                break;
            }
            if (event != 0)
                return event;
            *audio_status = fill_audio(audio, output, NX_AUDIO_PREBUFFER,
                                       &audio_ended);
            if (*audio_status != NX_AAC_OK)
                return 0;
            audio_clock_us = nx_pcm_output_clock_us(output);
        }
        if (seek_event != 0)
            goto perform_seek;
        if (audio_clock_us > target_us + frame_period_us) {
            stats->dropped++;
            continue;
        }

        if (*rb->current_tick < osd_until) {
            uint32_t current_second = (uint32_t)(audio_clock_us / 1000000u);
            draw_picture(video, playback_osd_y());
            if (current_second != osd_second) {
                draw_playback_osd(audio_clock_us, duration_us);
                osd_second = current_second;
            }
        } else {
            draw_picture(video, LCD_HEIGHT);
            osd_second = UINT32_MAX;
        }
        rb->lcd_update();
        stats->displayed++;
        event = process_button(rb->button_get(false), output);
        if (event == 3) {
            osd_until = *rb->current_tick + NX_OSD_SECONDS * HZ;
            draw_playback_osd(audio_clock_us, duration_us);
            rb->lcd_update();
            osd_second = (uint32_t)(audio_clock_us / 1000000u);
            event = 0;
        }
        if (event == 4 || event == 5) {
            seek_event = event;
            goto perform_seek;
        }
        if (event != 0)
            return event;
        rb->reset_poweroff_timer();
        continue;

perform_seek:
        {
            uint64_t current_us = nx_pcm_output_clock_us(output);
            uint64_t requested_us;
            if (seek_event == 4)
                requested_us = current_us > 10000000u ?
                               current_us - 10000000u : 0;
            else
                requested_us = current_us + 10000000u < duration_us ?
                               current_us + 10000000u : duration_us - 1;
            if (restart_av_at(video, audio, output, reader, track,
                              audio_track, requested_us, &audio_ended,
                              video_status, audio_status) != 0)
                return 0;
            osd_until = *rb->current_tick + NX_OSD_SECONDS * HZ;
            osd_second = UINT32_MAX;
            rb->reset_poweroff_timer();
        }
    }
}

enum plugin_status plugin_start(const void *parameter)
{
    struct rockbox_file file;
    struct nx_reader reader;
    struct nx_mp4_movie movie;
    enum nx_mp4_status status;
    enum nx_h264_status video_status = NX_H264_BAD_CONFIG;
    enum nx_aac_status audio_status = NX_AAC_BAD_CONFIG;
    enum nx_profile_status profile_status = NX_PROFILE_OK;
    struct nx_h264_decoder decoder;
    struct nx_aac_decoder audio_decoder;
    struct nx_pcm_output audio_output;
    void *decode_memory = NULL;
    size_t decode_memory_size = 0;
    size_t decode_memory_peak = 0;
    struct playback_stats playback;
    int playback_event = 0;
    off_t size;
    int button;
    int video_ready = 0;
    int audio_ready = 0;
    int output_ready = 0;

    if (parameter == NULL) {
        rb->splash(HZ * 2, "Select an MP4 file");
        return PLUGIN_ERROR;
    }

    file.fd = rb->open((const char *)parameter, O_RDONLY);
    if (file.fd < 0) {
        rb->splash(HZ * 2, "Cannot open file");
        return PLUGIN_ERROR;
    }

    size = rb->filesize(file.fd);
    if (size < 0) {
        rb->close(file.fd);
        rb->splash(HZ * 2, "Cannot read file size");
        return PLUGIN_ERROR;
    }

    reader.context = &file;
    reader.read_at = rockbox_read_at;
    reader.size = (uint64_t)size;
    status = nx_mp4_parse(&reader, &movie);
    if (status == NX_MP4_OK)
        profile_status = nx_mp4_validate_nx240(&movie);

    if (status == NX_MP4_OK && profile_status == NX_PROFILE_OK) {
        decode_memory = rb->plugin_get_buffer(&decode_memory_size);
        if (nx_h264_memory_init(decode_memory, decode_memory_size) == 0) {
            video_status = nx_h264_decoder_init(&decoder);
            if (video_status == NX_H264_OK) {
                video_ready = 1;
                audio_status = nx_aac_decoder_init(&audio_decoder, &reader,
                                                    &movie.audio);
            }
            if (video_status == NX_H264_OK && audio_status == NX_AAC_OK) {
                audio_ready = 1;
                if (nx_pcm_output_init(&audio_output,
                                       audio_decoder.sample_rate) == 0) {
                    output_ready = 1;
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
                    rb->cpu_boost(true);
#endif
                    backlight_force_on();
                    playback_event = play_av(&decoder, &audio_decoder,
                        &audio_output, &reader, &movie.video, &movie.audio,
                        &playback,
                        &video_status, &audio_status);
                    nx_pcm_output_stop(&audio_output);
                    backlight_use_settings();
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
                    rb->cpu_boost(false);
#endif
                } else {
                    audio_status = NX_AAC_NO_MEMORY;
                }
                decode_memory_peak = nx_h264_memory_peak();
                if (video_status == NX_H264_END_OF_STREAM &&
                    audio_status == NX_AAC_OK) {
                    draw_playback_stats(&playback, &audio_decoder,
                                        &audio_output, decode_memory_peak);
                    video_status = NX_H264_OK;
                }
            }
            if (output_ready)
                nx_pcm_output_destroy(&audio_output);
            if (audio_ready)
                nx_aac_decoder_destroy(&audio_decoder);
            if (video_ready)
                nx_h264_decoder_destroy(&decoder);
            if (video_status != NX_H264_OK)
                decode_memory_peak = nx_h264_memory_peak();
            nx_h264_memory_destroy();
        } else {
            video_status = NX_H264_NO_MEMORY;
        }
    }
    rb->close(file.fd);

    if (playback_event == 2)
        return PLUGIN_USB_CONNECTED;
    if (playback_event == 1)
        return PLUGIN_OK;

    if (status != NX_MP4_OK)
        draw_probe((const char *)parameter, status, &movie);
    else if (profile_status != NX_PROFILE_OK)
        draw_profile_error((const char *)parameter, profile_status);
    else if (video_status != NX_H264_OK)
        draw_decode_error((const char *)parameter, video_status,
                          audio_status,
                          decode_memory_size, decode_memory_peak);
    else if (audio_status != NX_AAC_OK)
        draw_decode_error((const char *)parameter, video_status,
                          audio_status, decode_memory_size,
                          decode_memory_peak);

    do {
        button = rb->button_get(true);
        if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
            return PLUGIN_USB_CONNECTED;
    } while (button == BUTTON_NONE);

    return status == NX_MP4_OK && profile_status == NX_PROFILE_OK &&
        video_status == NX_H264_OK &&
        audio_status == NX_AAC_OK ?
        PLUGIN_OK : PLUGIN_ERROR;
}
