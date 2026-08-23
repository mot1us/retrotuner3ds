#include "miniiptv/h264_annexb.h"

#include <stdbool.h>
#include <string.h>

static bool has_annexb_prefix(const uint8_t *data, size_t size) {
    return data && ((size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) ||
                    (size >= 4 && data[0] == 0 && data[1] == 0 &&
                     data[2] == 0 && data[3] == 1));
}

static int append_start_code_and_nal(uint8_t *output, size_t capacity,
                                     size_t *offset, const uint8_t *nal,
                                     size_t nal_size) {
    static const uint8_t start_code[3] = {0, 0, 1};

    if (!offset || !nal || nal_size == 0 || *offset > SIZE_MAX - 3 ||
        *offset + 3 > SIZE_MAX - nal_size)
        return MINIIPTV_H264_INVALID_DATA;
    if (output && *offset + 3 + nal_size > capacity)
        return MINIIPTV_H264_OUTPUT_TOO_SMALL;
    if (output) {
        memcpy(output + *offset, start_code, sizeof(start_code));
        memcpy(output + *offset + sizeof(start_code), nal, nal_size);
    }
    *offset += sizeof(start_code) + nal_size;
    return MINIIPTV_H264_OK;
}

static bool find_start_code(const uint8_t *data, size_t size, size_t from,
                            size_t *position, size_t *prefix_size) {
    size_t index;

    if (!data || !position || !prefix_size || from > size) return false;
    if (size < 3) return false;
    for (index = from; index <= size - 3; index++) {
        if (data[index] != 0 || data[index + 1] != 0) continue;
        if (size - index >= 4 && data[index + 2] == 0 &&
            data[index + 3] == 1) {
            *position = index;
            *prefix_size = 4;
            return true;
        }
        if (data[index + 2] == 1) {
            *position = index;
            *prefix_size = 3;
            return true;
        }
    }
    return false;
}

static int normalize_annexb(const uint8_t *data, size_t size, uint8_t *output,
                            size_t capacity, size_t *output_size) {
    size_t start;
    size_t prefix_size;
    size_t offset = 0;

    if (!data || !size || !output_size) return MINIIPTV_H264_INVALID_ARGUMENT;
    if (!find_start_code(data, size, 0, &start, &prefix_size) || start != 0)
        return MINIIPTV_H264_INVALID_DATA;

    while (start < size) {
        size_t nal_begin = start + prefix_size;
        size_t next = 0;
        size_t next_prefix = 0;
        size_t nal_end = size;
        bool has_next = find_start_code(data, size, nal_begin, &next,
                                        &next_prefix);
        int result;

        if (has_next) nal_end = next;
        if (nal_begin >= nal_end) return MINIIPTV_H264_INVALID_DATA;
        result = append_start_code_and_nal(output, capacity, &offset,
                                           data + nal_begin,
                                           nal_end - nal_begin);
        if (result != MINIIPTV_H264_OK) return result;
        if (!has_next) break;
        start = next;
        prefix_size = next_prefix;
    }

    *output_size = offset;
    return MINIIPTV_H264_OK;
}

int miniiptv_h264_extradata_to_annexb(const uint8_t *extradata,
                                      size_t extradata_size, uint8_t *output,
                                      size_t output_capacity,
                                      size_t *output_size) {
    size_t cursor;
    size_t offset = 0;
    unsigned int count;
    int result;

    if (!output_size || (!extradata && extradata_size))
        return MINIIPTV_H264_INVALID_ARGUMENT;
    *output_size = 0;
    if (!extradata_size) return MINIIPTV_H264_OK;
    if (has_annexb_prefix(extradata, extradata_size))
        return normalize_annexb(extradata, extradata_size, output,
                                output_capacity, output_size);
    if (extradata_size < 7 || extradata[0] != 1)
        return MINIIPTV_H264_INVALID_DATA;

    cursor = 6;
    count = extradata[5] & 0x1f;
    while (count--) {
        size_t nal_size;
        if (cursor + 2 > extradata_size) return MINIIPTV_H264_INVALID_DATA;
        nal_size = ((size_t)extradata[cursor] << 8) | extradata[cursor + 1];
        cursor += 2;
        if (!nal_size || nal_size > extradata_size - cursor)
            return MINIIPTV_H264_INVALID_DATA;
        result = append_start_code_and_nal(output, output_capacity, &offset,
                                           extradata + cursor, nal_size);
        if (result != MINIIPTV_H264_OK) return result;
        cursor += nal_size;
    }

    if (cursor >= extradata_size) return MINIIPTV_H264_INVALID_DATA;
    count = extradata[cursor++];
    while (count--) {
        size_t nal_size;
        if (cursor + 2 > extradata_size) return MINIIPTV_H264_INVALID_DATA;
        nal_size = ((size_t)extradata[cursor] << 8) | extradata[cursor + 1];
        cursor += 2;
        if (!nal_size || nal_size > extradata_size - cursor)
            return MINIIPTV_H264_INVALID_DATA;
        result = append_start_code_and_nal(output, output_capacity, &offset,
                                           extradata + cursor, nal_size);
        if (result != MINIIPTV_H264_OK) return result;
        cursor += nal_size;
    }

    if (!offset) return MINIIPTV_H264_INVALID_DATA;
    *output_size = offset;
    return MINIIPTV_H264_OK;
}

int miniiptv_h264_packet_to_annexb(const uint8_t *packet, size_t packet_size,
                                   const uint8_t *extradata,
                                   size_t extradata_size, uint8_t *output,
                                   size_t output_capacity,
                                   size_t *output_size) {
    size_t cursor = 0;
    size_t offset = 0;
    size_t length_size;

    if (!packet || !packet_size || !output_size ||
        (!extradata && extradata_size))
        return MINIIPTV_H264_INVALID_ARGUMENT;
    *output_size = 0;
    if (has_annexb_prefix(packet, packet_size))
        return normalize_annexb(packet, packet_size, output, output_capacity,
                                output_size);
    if (!extradata || extradata_size < 5 || extradata[0] != 1)
        return MINIIPTV_H264_INVALID_DATA;

    length_size = (extradata[4] & 3) + 1;
    while (cursor < packet_size) {
        size_t nal_size = 0;
        size_t index;
        int result;

        if (length_size > packet_size - cursor)
            return MINIIPTV_H264_INVALID_DATA;
        for (index = 0; index < length_size; index++)
            nal_size = (nal_size << 8) | packet[cursor + index];
        cursor += length_size;
        if (!nal_size || nal_size > packet_size - cursor)
            return MINIIPTV_H264_INVALID_DATA;
        result = append_start_code_and_nal(output, output_capacity, &offset,
                                           packet + cursor, nal_size);
        if (result != MINIIPTV_H264_OK) return result;
        cursor += nal_size;
    }

    if (!offset) return MINIIPTV_H264_INVALID_DATA;
    *output_size = offset;
    return MINIIPTV_H264_OK;
}
