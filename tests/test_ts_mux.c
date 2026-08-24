#include "miniiptv/ts_mux.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PACKET_SIZE 188u

static uint32_t crc32_mpeg(const unsigned char *data, size_t size) {
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t i = 0; i < size; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (unsigned int bit = 0; bit < 8u; bit++)
            crc = (crc & UINT32_C(0x80000000))
                ? (crc << 1) ^ UINT32_C(0x04c11db7) : crc << 1;
    }
    return crc;
}

static void finish_section(unsigned char *section, size_t size) {
    uint32_t crc = crc32_mpeg(section, size - 4u);
    section[size - 4u] = (unsigned char)(crc >> 24);
    section[size - 3u] = (unsigned char)(crc >> 16);
    section[size - 2u] = (unsigned char)(crc >> 8);
    section[size - 1u] = (unsigned char)crc;
}

static void make_psi_packet(unsigned char packet[PACKET_SIZE], uint16_t pid,
                            const unsigned char *section, size_t size) {
    memset(packet, 0xff, PACKET_SIZE);
    packet[0] = 0x47u;
    packet[1] = (unsigned char)(0x40u | ((pid >> 8) & 0x1fu));
    packet[2] = (unsigned char)pid;
    packet[3] = 0x10u;
    packet[4] = 0u;
    memcpy(packet + 5u, section, size);
}

static void make_pat(unsigned char packet[PACKET_SIZE], uint16_t pmt_pid) {
    unsigned char section[16] = {
        0x00u, 0xb0u, 0x0du, 0x00u, 0x01u, 0xc1u, 0x00u, 0x00u,
        0x00u, 0x01u, 0xe0u, 0x00u, 0u, 0u, 0u, 0u
    };
    section[10] = (unsigned char)(0xe0u | ((pmt_pid >> 8) & 0x1fu));
    section[11] = (unsigned char)pmt_pid;
    finish_section(section, sizeof(section));
    make_psi_packet(packet, 0u, section, sizeof(section));
}

static void make_pmt(unsigned char packet[PACKET_SIZE], uint16_t pmt_pid,
                     uint8_t stream_type, uint16_t elementary_pid) {
    unsigned char section[21] = {
        0x02u, 0xb0u, 0x12u, 0x00u, 0x01u, 0xc1u, 0x00u, 0x00u,
        0xe0u, 0x00u, 0xf0u, 0x00u,
        0x00u, 0xe0u, 0x00u, 0xf0u, 0x00u,
        0u, 0u, 0u, 0u
    };
    section[8] = (unsigned char)(0xe0u | ((elementary_pid >> 8) & 0x1fu));
    section[9] = (unsigned char)elementary_pid;
    section[12] = stream_type;
    section[13] = (unsigned char)(0xe0u | ((elementary_pid >> 8) & 0x1fu));
    section[14] = (unsigned char)elementary_pid;
    finish_section(section, sizeof(section));
    make_psi_packet(packet, pmt_pid, section, sizeof(section));
}

static void make_payload(unsigned char packet[PACKET_SIZE], uint16_t pid,
                         unsigned char value) {
    memset(packet, value, PACKET_SIZE);
    packet[0] = 0x47u;
    packet[1] = (unsigned char)((pid >> 8) & 0x1fu);
    packet[2] = (unsigned char)pid;
    packet[3] = 0x10u;
}

static uint16_t pid_of(const unsigned char *packet) {
    return (uint16_t)(((uint16_t)(packet[1] & 0x1fu) << 8) | packet[2]);
}

static void test_mux_adds_audio_to_video_program(void) {
    unsigned char video[3u * PACKET_SIZE];
    unsigned char audio[4u * PACKET_SIZE];
    unsigned char output[8u * PACKET_SIZE];
    size_t output_size = 0u;
    size_t audio_payload_packets = 0u;
    const unsigned char *pmt = NULL;

    make_pat(video, 0x1000u);
    make_pmt(video + PACKET_SIZE, 0x1000u, 0x1bu, 0x0100u);
    make_payload(video + 2u * PACKET_SIZE, 0x0100u, 0x55u);
    make_pat(audio, 0x1000u);
    make_pmt(audio + PACKET_SIZE, 0x1000u, 0x0fu, 0x0100u);
    make_payload(audio + 2u * PACKET_SIZE, 0x0100u, 0xaau);
    make_payload(audio + 3u * PACKET_SIZE, 0x0100u, 0xbbu);

    assert(miniiptv_ts_mux_av(video, sizeof(video), audio, sizeof(audio),
                             output, sizeof(output), &output_size) == 0);
    assert(output_size == 5u * PACKET_SIZE);
    for (size_t offset = 0; offset < output_size; offset += PACKET_SIZE) {
        uint16_t pid = pid_of(output + offset);
        if (pid == 0x0101u) audio_payload_packets++;
        if (pid == 0x1000u) pmt = output + offset;
    }
    assert(audio_payload_packets == 2u);
    assert(pmt != NULL);
    assert(pmt[5] == 0x02u);
    assert((((size_t)pmt[6] & 0x0fu) << 8 | pmt[7]) == 23u);
    assert(pmt[22] == 0x0fu);
    assert((((uint16_t)pmt[23] & 0x1fu) << 8 | pmt[24]) == 0x0101u);
    assert(crc32_mpeg(pmt + 5u, 26u) == 0u);
}

int main(void) {
    test_mux_adds_audio_to_video_program();
    puts("ts mux tests passed");
    return 0;
}
