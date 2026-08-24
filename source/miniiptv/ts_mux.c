#include "miniiptv/ts_mux.h"

#include <stdint.h>
#include <string.h>

#define TS_PACKET_SIZE 188u
#define TS_PID_COUNT 8192u
#define TS_NULL_PID 0x1fffu
#define TS_MAX_ES_INFO 96u

typedef struct {
    uint16_t pmt_pid;
    uint16_t pcr_pid;
    uint16_t audio_pid;
    uint8_t audio_type;
    unsigned char audio_descriptors[TS_MAX_ES_INFO];
    size_t audio_descriptor_size;
    int has_pmt;
    int has_audio;
} TsProgram;

static uint16_t packet_pid(const unsigned char *packet) {
    return (uint16_t)(((uint16_t)(packet[1] & 0x1fu) << 8) | packet[2]);
}

static void packet_set_pid(unsigned char *packet, uint16_t pid) {
    packet[1] = (unsigned char)((packet[1] & 0xe0u) |
                               ((pid >> 8) & 0x1fu));
    packet[2] = (unsigned char)(pid & 0xffu);
}

static const unsigned char *packet_section(const unsigned char *packet,
                                            size_t *available) {
    size_t offset = 4u;
    unsigned int adaptation_control;
    unsigned int pointer;
    if (!packet || !available || packet[0] != 0x47u ||
        (packet[1] & 0x40u) == 0)
        return NULL;
    adaptation_control = (packet[3] >> 4) & 3u;
    if (adaptation_control == 0u || adaptation_control == 2u) return NULL;
    if (adaptation_control == 3u) {
        offset += 1u + packet[4];
        if (offset >= TS_PACKET_SIZE) return NULL;
    }
    pointer = packet[offset++];
    if (pointer > TS_PACKET_SIZE - offset) return NULL;
    offset += pointer;
    if (offset >= TS_PACKET_SIZE) return NULL;
    *available = TS_PACKET_SIZE - offset;
    return packet + offset;
}

static int section_size(const unsigned char *section, size_t available,
                        size_t *size) {
    size_t declared;
    if (!section || !size || available < 3u) return -1;
    declared = 3u + (((size_t)section[1] & 0x0fu) << 8) + section[2];
    if (declared < 8u || declared > available) return -1;
    *size = declared;
    return 0;
}

static int find_pmt_pid(const unsigned char *data, size_t size,
                        uint16_t *pmt_pid) {
    if (!data || !pmt_pid || size == 0u || size % TS_PACKET_SIZE != 0u)
        return -1;
    for (size_t offset = 0; offset < size; offset += TS_PACKET_SIZE) {
        const unsigned char *packet = data + offset;
        const unsigned char *section;
        size_t available;
        size_t total;
        size_t cursor;
        if (packet_pid(packet) != 0u) continue;
        section = packet_section(packet, &available);
        if (!section || section[0] != 0x00u ||
            section_size(section, available, &total) != 0 || total < 12u)
            continue;
        cursor = 8u;
        while (cursor + 4u <= total - 4u) {
            uint16_t program =
                (uint16_t)(((uint16_t)section[cursor] << 8) |
                           section[cursor + 1u]);
            uint16_t pid =
                (uint16_t)(((uint16_t)(section[cursor + 2u] & 0x1fu) << 8) |
                           section[cursor + 3u]);
            if (program != 0u) {
                *pmt_pid = pid;
                return 0;
            }
            cursor += 4u;
        }
    }
    return -1;
}

static int is_audio_stream_type(unsigned int stream_type) {
    return stream_type == 0x03u || stream_type == 0x04u ||
           stream_type == 0x0fu || stream_type == 0x11u ||
           stream_type == 0x81u;
}

static int parse_pmt(const unsigned char *data, size_t size,
                     TsProgram *program, unsigned char used_pids[TS_PID_COUNT]) {
    if (!data || !program || size == 0u || size % TS_PACKET_SIZE != 0u)
        return -1;
    if (find_pmt_pid(data, size, &program->pmt_pid) != 0) return -1;
    if (used_pids) {
        memset(used_pids, 0, TS_PID_COUNT);
        used_pids[0] = 1u;
        used_pids[program->pmt_pid] = 1u;
    }
    for (size_t offset = 0; offset < size; offset += TS_PACKET_SIZE) {
        const unsigned char *packet = data + offset;
        const unsigned char *section;
        size_t available;
        size_t total;
        size_t program_info_size;
        size_t cursor;
        if (packet_pid(packet) != program->pmt_pid) continue;
        section = packet_section(packet, &available);
        if (!section || section[0] != 0x02u ||
            section_size(section, available, &total) != 0 || total < 16u)
            continue;
        program->pcr_pid =
            (uint16_t)(((uint16_t)(section[8] & 0x1fu) << 8) | section[9]);
        if (used_pids) used_pids[program->pcr_pid] = 1u;
        program_info_size =
            ((size_t)(section[10] & 0x0fu) << 8) | section[11];
        cursor = 12u + program_info_size;
        if (cursor > total - 4u) return -1;
        while (cursor + 5u <= total - 4u) {
            unsigned int stream_type = section[cursor];
            uint16_t pid =
                (uint16_t)(((uint16_t)(section[cursor + 1u] & 0x1fu) << 8) |
                           section[cursor + 2u]);
            size_t descriptor_size =
                ((size_t)(section[cursor + 3u] & 0x0fu) << 8) |
                section[cursor + 4u];
            if (cursor + 5u + descriptor_size > total - 4u) return -1;
            if (used_pids) used_pids[pid] = 1u;
            if (!program->has_audio && is_audio_stream_type(stream_type)) {
                if (descriptor_size > sizeof(program->audio_descriptors))
                    return -1;
                program->audio_pid = pid;
                program->audio_type = (uint8_t)stream_type;
                memcpy(program->audio_descriptors, section + cursor + 5u,
                       descriptor_size);
                program->audio_descriptor_size = descriptor_size;
                program->has_audio = 1;
            }
            cursor += 5u + descriptor_size;
        }
        program->has_pmt = 1;
        return 0;
    }
    return -1;
}

static uint32_t mpeg_crc32(const unsigned char *data, size_t size) {
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t i = 0; i < size; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (unsigned int bit = 0; bit < 8u; bit++)
            crc = (crc & UINT32_C(0x80000000))
                ? (crc << 1) ^ UINT32_C(0x04c11db7) : crc << 1;
    }
    return crc;
}

static int rewrite_video_pmt(unsigned char *packet,
                             const TsProgram *video_program,
                             const TsProgram *audio_program,
                             uint16_t output_audio_pid) {
    const unsigned char *old_section;
    unsigned char section[183];
    size_t available;
    size_t old_size;
    size_t old_without_crc;
    size_t entry_size;
    size_t new_size;
    size_t new_section_length;
    uint32_t crc;
    old_section = packet_section(packet, &available);
    if (!old_section || old_section[0] != 0x02u ||
        section_size(old_section, available, &old_size) != 0)
        return -1;
    entry_size = 5u + audio_program->audio_descriptor_size;
    old_without_crc = old_size - 4u;
    new_size = old_size + entry_size;
    if (new_size > sizeof(section)) return -1;
    memcpy(section, old_section, old_without_crc);
    section[old_without_crc] = audio_program->audio_type;
    section[old_without_crc + 1u] =
        (unsigned char)(0xe0u | ((output_audio_pid >> 8) & 0x1fu));
    section[old_without_crc + 2u] = (unsigned char)output_audio_pid;
    section[old_without_crc + 3u] =
        (unsigned char)(0xf0u |
            ((audio_program->audio_descriptor_size >> 8) & 0x0fu));
    section[old_without_crc + 4u] =
        (unsigned char)audio_program->audio_descriptor_size;
    memcpy(section + old_without_crc + 5u,
           audio_program->audio_descriptors,
           audio_program->audio_descriptor_size);
    new_section_length = new_size - 3u;
    section[1] = (unsigned char)((section[1] & 0xf0u) |
                                ((new_section_length >> 8) & 0x0fu));
    section[2] = (unsigned char)new_section_length;
    crc = mpeg_crc32(section, new_size - 4u);
    section[new_size - 4u] = (unsigned char)(crc >> 24);
    section[new_size - 3u] = (unsigned char)(crc >> 16);
    section[new_size - 2u] = (unsigned char)(crc >> 8);
    section[new_size - 1u] = (unsigned char)crc;

    memset(packet + 4u, 0xff, TS_PACKET_SIZE - 4u);
    packet[1] |= 0x40u;
    packet[3] = (unsigned char)((packet[3] & 0x0fu) | 0x10u);
    packet[4] = 0u;
    memcpy(packet + 5u, section, new_size);
    packet_set_pid(packet, video_program->pmt_pid);
    return 0;
}

int miniiptv_ts_mux_av(const unsigned char *video, size_t video_size,
                       const unsigned char *audio, size_t audio_size,
                       unsigned char *output, size_t output_capacity,
                       size_t *output_size) {
    TsProgram video_program = {0};
    TsProgram audio_program = {0};
    unsigned char used_pids[TS_PID_COUNT];
    uint16_t output_audio_pid;
    size_t video_packets;
    size_t audio_packets = 0u;
    size_t audio_cursor = 0u;
    size_t output_cursor = 0u;
    int rewrote_pmt = 0;
    if (output_size) *output_size = 0u;
    if (!video || !audio || !output || !output_size || video_size == 0u ||
        audio_size == 0u || video_size % TS_PACKET_SIZE != 0u ||
        audio_size % TS_PACKET_SIZE != 0u)
        return -1;
    if (parse_pmt(video, video_size, &video_program, used_pids) != 0 ||
        parse_pmt(audio, audio_size, &audio_program, NULL) != 0 ||
        !video_program.has_pmt || !audio_program.has_audio)
        return -2;
    output_audio_pid = audio_program.audio_pid;
    if (output_audio_pid == 0u || output_audio_pid == TS_NULL_PID ||
        used_pids[output_audio_pid]) {
        for (output_audio_pid = 0x0101u;
             output_audio_pid < TS_NULL_PID && used_pids[output_audio_pid];
             output_audio_pid++) {
        }
        if (output_audio_pid >= TS_NULL_PID) return -3;
    }
    video_packets = video_size / TS_PACKET_SIZE;
    for (size_t offset = 0; offset < audio_size; offset += TS_PACKET_SIZE) {
        if (packet_pid(audio + offset) == audio_program.audio_pid)
            audio_packets++;
    }
    if (audio_packets == 0u ||
        video_packets + audio_packets > output_capacity / TS_PACKET_SIZE)
        return -4;

    for (size_t video_index = 0; video_index < video_packets; video_index++) {
        const unsigned char *source = video + video_index * TS_PACKET_SIZE;
        unsigned char *destination = output + output_cursor;
        if (output_cursor > output_capacity - TS_PACKET_SIZE) return -4;
        memcpy(destination, source, TS_PACKET_SIZE);
        if (packet_pid(destination) == video_program.pmt_pid) {
            if (rewrite_video_pmt(destination, &video_program,
                                  &audio_program, output_audio_pid) == 0)
                rewrote_pmt = 1;
            else
                return -5;
        }
        output_cursor += TS_PACKET_SIZE;

        while (audio_cursor < audio_size &&
               (output_cursor / TS_PACKET_SIZE - video_index - 1u) <
                   ((video_index + 1u) * audio_packets) / video_packets) {
            const unsigned char *audio_packet = audio + audio_cursor;
            audio_cursor += TS_PACKET_SIZE;
            if (packet_pid(audio_packet) != audio_program.audio_pid) continue;
            if (output_cursor > output_capacity - TS_PACKET_SIZE) return -4;
            memcpy(output + output_cursor, audio_packet, TS_PACKET_SIZE);
            packet_set_pid(output + output_cursor, output_audio_pid);
            output_cursor += TS_PACKET_SIZE;
        }
    }
    while (audio_cursor < audio_size) {
        const unsigned char *audio_packet = audio + audio_cursor;
        audio_cursor += TS_PACKET_SIZE;
        if (packet_pid(audio_packet) != audio_program.audio_pid) continue;
        if (output_cursor > output_capacity - TS_PACKET_SIZE) return -4;
        memcpy(output + output_cursor, audio_packet, TS_PACKET_SIZE);
        packet_set_pid(output + output_cursor, output_audio_pid);
        output_cursor += TS_PACKET_SIZE;
    }
    if (!rewrote_pmt) return -5;
    *output_size = output_cursor;
    return 0;
}
