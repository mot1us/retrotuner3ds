#ifndef MINIIPTV_TS_MUX_H
#define MINIIPTV_TS_MUX_H

#include <stddef.h>

/* Combine one aligned video-only MPEG-TS segment and one audio-only MPEG-TS
 * segment into a single transport stream. The function rewrites the video
 * program map to advertise the selected audio elementary stream, remaps its
 * PID if needed, and distributes audio packets through the video segment.
 * It never emits a partial result. */
int miniiptv_ts_mux_av(const unsigned char *video, size_t video_size,
                       const unsigned char *audio, size_t audio_size,
                       unsigned char *output, size_t output_capacity,
                       size_t *output_size);

#endif
