#ifndef RETROTUNER_TEST_3DS_H
#define RETROTUNER_TEST_3DS_H

/* Only the NDSP interface used by speaker.c. These fakes test our handoff
 * contract; they do not emulate the DSP, cache hardware, or thread timing. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int32_t Result;
typedef struct {
    void *data_vaddr;
    uint32_t nsamples;
    uint8_t status;
} ndspWaveBuf;

enum { NDSP_WBUF_FREE, NDSP_WBUF_QUEUED, NDSP_WBUF_PLAYING, NDSP_WBUF_DONE };
enum { NDSP_FORMAT_MONO_PCM16, NDSP_FORMAT_STEREO_PCM16 };
enum { NDSP_OUTPUT_MONO, NDSP_OUTPUT_STEREO, NDSP_INTERP_LINEAR };

Result ndspInit(void);
void ndspExit(void);
void ndspChnReset(int channel);
void ndspChnWaveBufClear(int channel);
void ndspChnSetMix(int channel, const float *mix);
void ndspChnSetFormat(int channel, uint16_t format);
void ndspSetOutputMode(int mode);
void ndspChnSetInterp(int channel, int interpolation);
void ndspChnSetRate(int channel, float rate);
void ndspChnWaveBufAdd(int channel, ndspWaveBuf *wave);
uint32_t ndspChnGetSamplePos(int channel);
void ndspChnSetPaused(int channel, bool paused);
bool ndspChnIsPaused(int channel);
bool ndspChnIsPlaying(int channel);
void *linearAlloc(size_t size);
Result DSP_FlushDataCache(const void *address, uint32_t size);

#endif
