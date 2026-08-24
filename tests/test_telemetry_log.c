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
    miniiptv_telemetry_log_record(1000, &sample);
    miniiptv_telemetry_log_record(1500, &sample);
    miniiptv_telemetry_log_record(2000, &sample);
    sample.shadow_state = "FILL";
    sample.global_underruns = 1u;
    sample.total_underruns = 1u;
    miniiptv_telemetry_log_record(2100, &sample);
    sample.last_error = -7;
    miniiptv_telemetry_log_record(2200, &sample);
    miniiptv_telemetry_log_close();

    data = read_file(path, NULL);
    assert(strstr(data, "version,elapsed_ms,event,channel") != NULL);
    assert(strstr(data, ",START,") != NULL);
    assert(strstr(data, ",SAMPLE,") != NULL);
    assert(strstr(data, ",UNDERRUN,") != NULL);
    assert(strstr(data, ",ERROR,") != NULL);
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
