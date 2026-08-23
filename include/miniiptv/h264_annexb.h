#ifndef MINIIPTV_H264_ANNEXB_H
#define MINIIPTV_H264_ANNEXB_H

#include <stddef.h>
#include <stdint.h>

enum {
    MINIIPTV_H264_OK = 0,
    MINIIPTV_H264_INVALID_ARGUMENT = -1,
    MINIIPTV_H264_INVALID_DATA = -2,
    MINIIPTV_H264_OUTPUT_TOO_SMALL = -3,
    MINIIPTV_H264_PARAMETER_CHANGED = -4,
    MINIIPTV_H264_PARAMETER_LIMIT = -5,
    MINIIPTV_H264_PARAMETERS_MISSING = -6
};

#define MINIIPTV_H264_MAX_PARAMETER_SETS 1
#define MINIIPTV_H264_MAX_PARAMETER_BYTES 512

typedef struct {
    uint8_t sps[MINIIPTV_H264_MAX_PARAMETER_SETS]
               [MINIIPTV_H264_MAX_PARAMETER_BYTES];
    uint8_t pps[MINIIPTV_H264_MAX_PARAMETER_SETS]
               [MINIIPTV_H264_MAX_PARAMETER_BYTES];
    size_t sps_sizes[MINIIPTV_H264_MAX_PARAMETER_SETS];
    size_t pps_sizes[MINIIPTV_H264_MAX_PARAMETER_SETS];
    unsigned int sps_count;
    unsigned int pps_count;
    int sps_locked;
    int pps_locked;
} MiniIptvH264ParameterGuard;

/*
 * Normalize an H.264 packet for the 3DS MVD service. Annex-B input is parsed
 * and every NAL is rewritten with the exact 00 00 01 prefix required by MVD.
 * Length-prefixed input is converted using the length size stored in an
 * AVCDecoderConfigurationRecord (avcC).
 *
 * Pass output == NULL to validate and query the required output size.
 */
int miniiptv_h264_packet_to_annexb(const uint8_t *packet, size_t packet_size,
                                   const uint8_t *extradata,
                                   size_t extradata_size, uint8_t *output,
                                   size_t output_capacity,
                                   size_t *output_size);

/*
 * Extract SPS/PPS parameter sets from avcC metadata, or normalize Annex-B
 * extradata for MVD. Empty metadata is valid and produces zero bytes because
 * MPEG-TS commonly carries SPS/PPS in-band.
 */
int miniiptv_h264_extradata_to_annexb(const uint8_t *extradata,
                                      size_t extradata_size, uint8_t *output,
                                      size_t output_capacity,
                                      size_t *output_size);

/* Record exactly one SPS/PPS baseline, reject any distinct set, and reject VCL
 * data until both parameter types are known. Input must be Annex-B. */
void miniiptv_h264_parameter_guard_reset(
    MiniIptvH264ParameterGuard *guard);
int miniiptv_h264_parameter_guard_check(
    MiniIptvH264ParameterGuard *guard, const uint8_t *annexb,
    size_t annexb_size);

#endif
