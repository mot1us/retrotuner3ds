#include "miniiptv/h264_annexb.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_empty_extradata(void) {
    size_t size = 99;
    assert(miniiptv_h264_extradata_to_annexb(NULL, 0, NULL, 0, &size) ==
           MINIIPTV_H264_OK);
    assert(size == 0);
}

static void test_annexb_packet(void) {
    const uint8_t packet[] = {0, 0, 0, 1, 9, 16, 0, 0, 1, 0x67};
    const uint8_t expected[] = {0, 0, 1, 9, 16, 0, 0, 1, 0x67};
    uint8_t output[sizeof(packet)] = {0};
    size_t size = 0;
    assert(miniiptv_h264_packet_to_annexb(packet, sizeof(packet), NULL, 0,
                                          NULL, 0, &size) == MINIIPTV_H264_OK);
    assert(size == sizeof(expected));
    assert(miniiptv_h264_packet_to_annexb(packet, sizeof(packet), NULL, 0,
                                          output, sizeof(output), &size) ==
           MINIIPTV_H264_OK);
    assert(size == sizeof(expected));
    assert(memcmp(expected, output, sizeof(expected)) == 0);
}

static void test_captured_transport_stream_prefix(void) {
    /* TVS Turbo's first demuxed packet begins with AUD then an in-band SPS. */
    const uint8_t packet[] = {
        0x00, 0x00, 0x00, 0x01, 0x09, 0x10,
        0x00, 0x00, 0x01, 0x67, 0x4d, 0x40, 0x28
    };
    const uint8_t expected[] = {
        0x00, 0x00, 0x01, 0x09, 0x10,
        0x00, 0x00, 0x01, 0x67, 0x4d, 0x40, 0x28
    };
    uint8_t output[sizeof(packet)] = {0};
    size_t size = 0;

    assert(miniiptv_h264_packet_to_annexb(packet, sizeof(packet), NULL, 0,
                                          NULL, 0, &size) == MINIIPTV_H264_OK);
    assert(size == sizeof(expected));
    assert(miniiptv_h264_packet_to_annexb(packet, sizeof(packet), NULL, 0,
                                          output, sizeof(output), &size) ==
           MINIIPTV_H264_OK);
    assert(size == sizeof(expected));
    assert(memcmp(expected, output, sizeof(expected)) == 0);
}

static void test_avcc_extradata_and_packet(void) {
    const uint8_t avcc[] = {
        1, 0x4d, 0x40, 0x28, 0xff, 0xe1, 0, 3, 0x67, 0x11, 0x22,
        1, 0, 2, 0x68, 0x33
    };
    const uint8_t packet[] = {0, 0, 0, 2, 0x65, 0xaa,
                              0, 0, 0, 1, 0x09};
    const uint8_t expected_extra[] = {0, 0, 1, 0x67, 0x11, 0x22,
                                      0, 0, 1, 0x68, 0x33};
    const uint8_t expected_packet[] = {0, 0, 1, 0x65, 0xaa,
                                       0, 0, 1, 0x09};
    uint8_t output[32] = {0};
    size_t size = 0;

    assert(miniiptv_h264_extradata_to_annexb(avcc, sizeof(avcc), output,
                                              sizeof(output), &size) ==
           MINIIPTV_H264_OK);
    assert(size == sizeof(expected_extra));
    assert(memcmp(output, expected_extra, size) == 0);
    memset(output, 0, sizeof(output));
    assert(miniiptv_h264_packet_to_annexb(packet, sizeof(packet), avcc,
                                          sizeof(avcc), output,
                                          sizeof(output), &size) ==
           MINIIPTV_H264_OK);
    assert(size == sizeof(expected_packet));
    assert(memcmp(output, expected_packet, size) == 0);
}

static void test_malformed_and_capacity(void) {
    const uint8_t bad_packet[] = {0, 0, 0, 8, 0x65};
    const uint8_t avcc[] = {1, 0x4d, 0x40, 0x28, 0xff, 0xe0, 0};
    const uint8_t annexb[] = {0, 0, 1, 0x65};
    uint8_t output[3];
    size_t size = 0;

    assert(miniiptv_h264_packet_to_annexb(bad_packet, sizeof(bad_packet), avcc,
                                          sizeof(avcc), NULL, 0, &size) ==
           MINIIPTV_H264_INVALID_DATA);
    assert(miniiptv_h264_packet_to_annexb(annexb, sizeof(annexb), NULL, 0,
                                          output, sizeof(output), &size) ==
           MINIIPTV_H264_OUTPUT_TOO_SMALL);
}

static uint32_t fuzz_next(uint32_t *state) {
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static void test_malformed_input_safety(void) {
    uint8_t packet[64];
    uint8_t extradata[32];
    uint8_t output[256];
    uint32_t state = 0x6d766431u;
    size_t iteration;

    for (iteration = 0; iteration < 20000; iteration++) {
        size_t packet_size = (fuzz_next(&state) % sizeof(packet)) + 1;
        size_t extradata_size = fuzz_next(&state) % (sizeof(extradata) + 1);
        size_t index;
        size_t required = 0;
        int result;

        for (index = 0; index < packet_size; index++)
            packet[index] = (uint8_t)fuzz_next(&state);
        for (index = 0; index < extradata_size; index++)
            extradata[index] = (uint8_t)fuzz_next(&state);

        result = miniiptv_h264_packet_to_annexb(
            packet, packet_size, extradata_size ? extradata : NULL,
            extradata_size, NULL, 0, &required);
        if (result == MINIIPTV_H264_OK) {
            assert(required <= sizeof(output));
            assert(miniiptv_h264_packet_to_annexb(
                       packet, packet_size,
                       extradata_size ? extradata : NULL, extradata_size,
                       output, sizeof(output), &required) == MINIIPTV_H264_OK);
        }

        result = miniiptv_h264_extradata_to_annexb(
            extradata_size ? extradata : NULL, extradata_size, NULL, 0,
            &required);
        if (result == MINIIPTV_H264_OK) {
            assert(required <= sizeof(output));
            assert(miniiptv_h264_extradata_to_annexb(
                       extradata_size ? extradata : NULL, extradata_size,
                       output, sizeof(output), &required) == MINIIPTV_H264_OK);
        }
    }
}

int main(void) {
    test_empty_extradata();
    test_annexb_packet();
    test_captured_transport_stream_prefix();
    test_avcc_extradata_and_packet();
    test_malformed_and_capacity();
    test_malformed_input_safety();
    puts("h264_annexb tests passed");
    return 0;
}
