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

static void test_annexb_nal_iterator(void) {
    const uint8_t normalized[] = {
        0, 0, 1, 9, 0x10,
        0, 0, 1, 0x67, 0x42, 0xc0, 0x1e,
        0, 0, 1, 0x68, 0xce, 0x3c, 0x80
    };
    const size_t expected_sizes[] = {5, 7, 7};
    const unsigned int expected_types[] = {9, 7, 8};
    const uint8_t mixed_prefixes[] = {
        0, 0, 0, 1, 9, 0x10,
        0, 0, 1, 0x65, 0x88
    };
    const uint8_t leading_garbage[] = {0xff, 0, 0, 1, 9};
    const uint8_t empty_nal[] = {0, 0, 1, 0, 0, 1, 9};
    const uint8_t *nal = NULL;
    size_t nal_size = 0;
    size_t cursor = 0;

    for (size_t index = 0; index < 3; index++) {
        assert(miniiptv_h264_annexb_next_nal(
                   normalized, sizeof(normalized), &cursor, &nal,
                   &nal_size) == MINIIPTV_H264_NAL_ITER_FOUND);
        assert(nal != NULL);
        assert(nal_size == expected_sizes[index]);
        assert(nal[0] == 0 && nal[1] == 0 && nal[2] == 1);
        assert((nal[3] & 0x1f) == expected_types[index]);
    }
    assert(cursor == sizeof(normalized));
    assert(miniiptv_h264_annexb_next_nal(
               normalized, sizeof(normalized), &cursor, &nal,
               &nal_size) == MINIIPTV_H264_NAL_ITER_END);
    assert(nal == NULL && nal_size == 0);

    cursor = 0;
    assert(miniiptv_h264_annexb_next_nal(
               mixed_prefixes, sizeof(mixed_prefixes), &cursor, &nal,
               &nal_size) == MINIIPTV_H264_NAL_ITER_FOUND);
    assert(nal_size == 6 && nal[3] == 1 && (nal[4] & 0x1f) == 9);
    assert(miniiptv_h264_annexb_next_nal(
               mixed_prefixes, sizeof(mixed_prefixes), &cursor, &nal,
               &nal_size) == MINIIPTV_H264_NAL_ITER_FOUND);
    assert(nal_size == 5 && nal[2] == 1 && (nal[3] & 0x1f) == 5);

    cursor = 0;
    assert(miniiptv_h264_annexb_next_nal(
               leading_garbage, sizeof(leading_garbage), &cursor, &nal,
               &nal_size) == MINIIPTV_H264_INVALID_DATA);
    cursor = 0;
    assert(miniiptv_h264_annexb_next_nal(
               empty_nal, sizeof(empty_nal), &cursor, &nal,
               &nal_size) == MINIIPTV_H264_INVALID_DATA);
    cursor = sizeof(normalized) + 1;
    assert(miniiptv_h264_annexb_next_nal(
               normalized, sizeof(normalized), &cursor, &nal,
               &nal_size) == MINIIPTV_H264_INVALID_ARGUMENT);
}

static void test_annexb_nal_resume_cursor(void) {
    const uint8_t access_unit[] = {
        0, 0, 1, 0x09, 0x10,
        0, 0, 1, 0x06, 0x05, 0xff,
        0, 0, 1, 0x65, 0x88, 0x84,
        0, 0, 1, 0x0c, 0x80
    };
    const unsigned int expected_tail_types[] = {6, 5, 12};
    const uint8_t *nal = NULL;
    size_t nal_size = 0;
    size_t cursor = 0;
    size_t resume_cursor;

    /* Model an MVD render boundary after AUD: the saved cursor must resume at
     * SEI, not replay AUD or skip directly to the slice. */
    assert(miniiptv_h264_annexb_next_nal(
               access_unit, sizeof(access_unit), &cursor, &nal,
               &nal_size) == MINIIPTV_H264_NAL_ITER_FOUND);
    assert((nal[3] & 0x1f) == 9);
    resume_cursor = cursor;
    assert(resume_cursor == 5);

    cursor = resume_cursor;
    for (size_t index = 0; index < 3; index++) {
        assert(miniiptv_h264_annexb_next_nal(
                   access_unit, sizeof(access_unit), &cursor, &nal,
                   &nal_size) == MINIIPTV_H264_NAL_ITER_FOUND);
        assert((nal[3] & 0x1f) == expected_tail_types[index]);
    }
    assert(cursor == sizeof(access_unit));
    assert(miniiptv_h264_annexb_next_nal(
               access_unit, sizeof(access_unit), &cursor, &nal,
               &nal_size) == MINIIPTV_H264_NAL_ITER_END);
}

static void test_parameter_guard(void) {
    const uint8_t baseline[] = {
        0, 0, 1, 0x67, 0x4d, 0x40, 0x1e,
        0, 0, 1, 0x68, 0xee, 0x3c, 0x80
    };
    const uint8_t repeated[] = {
        0, 0, 0, 1, 0x67, 0x4d, 0x40, 0x1e,
        0, 0, 1, 0x68, 0xee, 0x3c, 0x80,
        0, 0, 1, 0x65, 0x88
    };
    const uint8_t changed_sps[] = {
        0, 0, 1, 0x67, 0x64, 0x00, 0x28,
        0, 0, 1, 0x65, 0x88
    };
    const uint8_t changed_pps[] = {
        0, 0, 1, 0x68, 0xee, 0x3d, 0x80
    };
    const uint8_t equivalent_padded[] = {
        0, 0, 0, 1, 0x27, 0x4d, 0x40, 0x1e, 0, 0,
        0, 0, 1, 0x28, 0xee, 0x3c, 0x80, 0, 0
    };
    const uint8_t multiple_initial_sets[] = {
        0, 0, 1, 0x67, 0x42, 0x00, 0x1e,
        0, 0, 1, 0x67, 0x4d, 0x00, 0x1e,
        0, 0, 1, 0x68, 0xce, 0x06, 0xe2,
        0, 0, 1, 0x68, 0xee, 0x3c, 0x80
    };
    const uint8_t first_sps_only[] = {
        0, 0, 1, 0x67, 0x42, 0x00, 0x1e, 0xe9
    };
    const uint8_t first_pps_only[] = {
        0, 0, 1, 0x68, 0xce, 0x06, 0xe2
    };
    const uint8_t baseline_with_slice[] = {
        0, 0, 1, 0x67, 0x4d, 0x40, 0x1e,
        0, 0, 1, 0x68, 0xee, 0x3c, 0x80,
        0, 0, 1, 0x65, 0x88
    };
    const uint8_t slice_before_baseline[] = {
        0, 0, 1, 0x65, 0x88,
        0, 0, 1, 0x67, 0x4d, 0x40, 0x1e,
        0, 0, 1, 0x68, 0xee, 0x3c, 0x80
    };
    const uint8_t second_sps_before_pps[] = {
        0, 0, 1, 0x67, 0x4d, 0x00, 0x1e, 0xe9
    };
    const uint8_t slices_only[] = {0, 0, 1, 0x65, 0x88};
    const uint8_t invalid_header[] = {0, 0, 1, 0xe7, 0x11};
    const uint8_t reserved_type[] = {0, 0, 1, 0x76, 0x11};
    MiniIptvH264ParameterGuard guard;

    miniiptv_h264_parameter_guard_reset(&guard);
    assert(miniiptv_h264_parameter_guard_check(
               &guard, slices_only, sizeof(slices_only)) ==
           MINIIPTV_H264_PARAMETERS_MISSING);
    assert(!guard.sps_locked && !guard.pps_locked);
    assert(miniiptv_h264_parameter_guard_check(
               &guard, slice_before_baseline,
               sizeof(slice_before_baseline)) ==
           MINIIPTV_H264_PARAMETERS_MISSING);
    assert(guard.sps_count == 0 && guard.pps_count == 0);
    assert(miniiptv_h264_parameter_guard_check(
               &guard, baseline_with_slice, sizeof(baseline_with_slice)) ==
           MINIIPTV_H264_OK);

    miniiptv_h264_parameter_guard_reset(&guard);
    assert(miniiptv_h264_parameter_guard_check(
               &guard, baseline, sizeof(baseline)) == MINIIPTV_H264_OK);
    assert(guard.sps_locked && guard.pps_locked);
    assert(guard.sps_count == 1 && guard.pps_count == 1);
    assert(miniiptv_h264_parameter_guard_check(
               &guard, repeated, sizeof(repeated)) == MINIIPTV_H264_OK);
    assert(miniiptv_h264_parameter_guard_check(
               &guard, equivalent_padded, sizeof(equivalent_padded)) ==
           MINIIPTV_H264_OK);
    assert(miniiptv_h264_parameter_guard_check(
               &guard, changed_sps, sizeof(changed_sps)) ==
           MINIIPTV_H264_PARAMETER_CHANGED);
    assert(miniiptv_h264_parameter_guard_check(
               &guard, changed_pps, sizeof(changed_pps)) ==
           MINIIPTV_H264_PARAMETER_CHANGED);

    miniiptv_h264_parameter_guard_reset(&guard);
    assert(miniiptv_h264_parameter_guard_check(
               &guard, multiple_initial_sets,
               sizeof(multiple_initial_sets)) ==
           MINIIPTV_H264_PARAMETER_CHANGED);
    assert(guard.sps_count == 1 && guard.pps_count == 0);

    miniiptv_h264_parameter_guard_reset(&guard);
    assert(miniiptv_h264_parameter_guard_check(
               &guard, first_sps_only, sizeof(first_sps_only)) ==
           MINIIPTV_H264_OK);
    assert(guard.sps_locked && !guard.pps_locked);
    assert(miniiptv_h264_parameter_guard_check(
               &guard, slices_only, sizeof(slices_only)) ==
           MINIIPTV_H264_PARAMETERS_MISSING);
    assert(miniiptv_h264_parameter_guard_check(
               &guard, second_sps_before_pps,
               sizeof(second_sps_before_pps)) ==
           MINIIPTV_H264_PARAMETER_CHANGED);

    miniiptv_h264_parameter_guard_reset(&guard);
    assert(miniiptv_h264_parameter_guard_check(
               &guard, first_pps_only, sizeof(first_pps_only)) ==
           MINIIPTV_H264_OK);
    assert(miniiptv_h264_parameter_guard_check(
               &guard, slices_only, sizeof(slices_only)) ==
           MINIIPTV_H264_PARAMETERS_MISSING);

    miniiptv_h264_parameter_guard_reset(&guard);
    assert(miniiptv_h264_parameter_guard_check(
               &guard, invalid_header, sizeof(invalid_header)) ==
           MINIIPTV_H264_INVALID_DATA);
    miniiptv_h264_parameter_guard_reset(&guard);
    assert(miniiptv_h264_parameter_guard_check(
               &guard, reserved_type, sizeof(reserved_type)) ==
           MINIIPTV_H264_INVALID_DATA);
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
    test_annexb_nal_iterator();
    test_annexb_nal_resume_cursor();
    test_parameter_guard();
    test_malformed_input_safety();
    puts("h264_annexb tests passed");
    return 0;
}
