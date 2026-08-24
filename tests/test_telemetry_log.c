#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniiptv/telemetry_log.h"

static char *read_file(const char *path, long *size_out) {
    FILE *file = fopen(path, "rb");
    char *data;
    long size;
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    size = ftell(file);
    assert(size >= 0);
    assert(fseek(file, 0, SEEK_SET) == 0);
    data = malloc((size_t)size + 1u);
    assert(data);
    assert(fread(data, 1, (size_t)size, file) == (size_t)size);
    data[size] = '\0';
    assert(fclose(file) == 0);
    if (size_out) *size_out = size;
    return data;
}

static unsigned int count_text(const char *haystack, const char *needle) {
    unsigned int count = 0;
    size_t length = strlen(needle);
    while (length > 0 && (haystack = strstr(haystack, needle)) != NULL) {
        count++;
        haystack += length;
    }
    return count;
}

static void test_rows_and_events(void) {
    const char *path = "tests/bin/test_telemetry_log.csv";
    MiniIptvTelemetrySample sample = {0};
    char *data;

    (void)remove(path);
    assert(miniiptv_telemetry_log_open(path, "0.5-test", 1000) == 0);
    sample.channel_name = "Retro, \"One\"\n";
    sample.app_state = "PLAYBACK";
    sample.tune_phase = "READY";
    sample.shadow_state = "GOOD";
    sample.producer_state = "LIVE EDGE";
    sample.periodic = true;
    sample.ring_bytes = 512u;
    sample.headroom_permille = 1800u;
    sample.rebuffer_target_bytes = 524288u;
    sample.rebuffer_wait_limit_ms = 6000u;
    sample.app_region_total_bytes = 130023424u;
    sample.heap_total_bytes = 5242880u;
    sample.heap_used_bytes = 1048576u;
    sample.linear_total_bytes = 104857600u;
    sample.linear_free_bytes = 73400320u;
    sample.video_packets = 101u;
    sample.video_decoded_frames = 99u;
    sample.video_textures = 98u;
    sample.video_presented_frames = 97u;
    sample.audio_tracks = 1u;
    sample.audio_state = 3u;
    sample.audio_demux_packets = 202u;
    sample.audio_frames = 199u;
    sample.audio_buffers = 198u;
    sample.audio_last_error = 0x1234u;
    miniiptv_telemetry_log_record(1000, &sample);
    miniiptv_telemetry_log_record(1500, &sample);
    miniiptv_telemetry_log_record(3000, &sample);
    sample.shadow_state = "FILL";
    sample.global_underruns = 1u;
    sample.total_underruns = 1u;
    miniiptv_telemetry_log_record(3100, &sample);
    sample.last_error = -7;
    miniiptv_telemetry_log_record(3200, &sample);
    sample.last_error = -12;
    miniiptv_telemetry_log_record(4000, &sample);
    miniiptv_telemetry_log_close();

    data = read_file(path, NULL);
    assert(strstr(data, "version,elapsed_ms,event,channel") != NULL);
    assert(strstr(data, "want_ms,rebuffer_target_bytes,"
                        "rebuffer_wait_limit_ms,lag_segments") != NULL);
    assert(strstr(data, "app_region_total_bytes,heap_total_bytes,"
                        "heap_used_bytes,linear_total_bytes,"
                        "linear_free_bytes,video_packets,") != NULL);
    assert(strstr(data, "video_presented_frames,audio_tracks,audio_state,"
                        "audio_demux_packets,audio_frames,audio_buffers,"
                        "audio_last_error,last_error") != NULL);
    assert(strstr(data, ",130023424,5242880,1048576,104857600,"
                        "73400320,101,99,98,97,1,3,202,199,198,4660,0\n")
           != NULL);
    assert(strstr(data, ",START,") != NULL);
    assert(strstr(data, ",SAMPLE,") != NULL);
    assert(strstr(data, ",UNDERRUN,") != NULL);
    assert(count_text(data, ",ERROR,") == 1u);
    assert(strstr(data, ",-12\n") == NULL);
    assert(strstr(data, ",0\n") != NULL);
    assert(strstr(data, "\"Retro, \"\"One\"\" \"") != NULL);
    free(data);
    assert(remove(path) == 0);
}

static void test_file_cap(void) {
    const char *path = "tests/bin/test_telemetry_log_cap.csv";
    MiniIptvTelemetrySample sample = {0};
    char channel[96];
    long size;
    char *data;
    unsigned int index;

    (void)remove(path);
    assert(miniiptv_telemetry_log_open(path, "0.5-test", 0) == 0);
    sample.app_state = "PLAYBACK";
    sample.tune_phase = "READY";
    sample.shadow_state = "GOOD";
    sample.producer_state = "SEGMENT";
    for (index = 0; index < 10000u &&
         miniiptv_telemetry_log_is_enabled(); index++) {
        snprintf(channel, sizeof(channel),
                 "Long diagnostic channel %u, repeated to reach the cap", index);
        sample.channel_name = channel;
        sample.periodic = true;
        miniiptv_telemetry_log_record((uint64_t)index * 1000u, &sample);
    }
    assert(!miniiptv_telemetry_log_is_enabled());
    miniiptv_telemetry_log_close();
    data = read_file(path, &size);
    assert(size > 400000);
    assert((unsigned long)size <= MINIIPTV_TELEMETRY_LOG_MAX_BYTES);
    free(data);
    assert(remove(path) == 0);
}

int main(void) {
    test_rows_and_events();
    test_file_cap();
    assert(miniiptv_telemetry_log_open(
               "tests/bin/missing/directory/telemetry.csv", "x", 0) != 0);
    miniiptv_telemetry_log_close();
    puts("telemetry log tests passed");
    return 0;
}
