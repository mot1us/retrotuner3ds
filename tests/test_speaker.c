#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "3ds.h"
#include "system/util/err_types.h"
#include "system/util/speaker.h"

static ndspWaveBuf *queued[24][DEF_SPEAKER_MAX_BUFFERS];
static const uint8_t pcm[] = { 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0 };
static ndspWaveBuf *last_wave;
static const void *flushed_address;
static uint32_t flushed_bytes;
static unsigned flush_calls, add_calls, alloc_calls;
static Result flush_result;
static bool allocation_fails, paused[24], initialized;
static uint32_t sample_position;

Result ndspInit(void) { initialized = true; return 0; }
void ndspExit(void) { initialized = false; }
void ndspChnReset(int channel) { ndspChnWaveBufClear(channel); }
void ndspChnWaveBufClear(int channel)
{
    for (unsigned i = 0; i < DEF_SPEAKER_MAX_BUFFERS; ++i) {
        if (queued[channel][i]) queued[channel][i]->status = NDSP_WBUF_FREE;
        queued[channel][i] = NULL;
    }
}
void ndspChnSetMix(int channel, const float *mix) { (void)channel; (void)mix; }
void ndspChnSetFormat(int channel, uint16_t format) { (void)channel; (void)format; }
void ndspSetOutputMode(int mode) { (void)mode; }
void ndspChnSetInterp(int channel, int interpolation) { (void)channel; (void)interpolation; }
void ndspChnSetRate(int channel, float rate) { (void)channel; (void)rate; }
uint32_t ndspChnGetSamplePos(int channel) { (void)channel; return sample_position; }
void ndspChnSetPaused(int channel, bool value) { paused[channel] = value; }
bool ndspChnIsPaused(int channel) { return paused[channel]; }
bool ndspChnIsPlaying(int channel)
{
    for (unsigned i = 0; i < DEF_SPEAKER_MAX_BUFFERS; ++i)
        if (queued[channel][i] && queued[channel][i]->status == NDSP_WBUF_PLAYING)
            return true;
    return false;
}
void *linearAlloc(size_t size)
{
    ++alloc_calls;
    return allocation_fails ? NULL : malloc(size);
}
Result DSP_FlushDataCache(const void *address, uint32_t size)
{
    ++flush_calls;
    /* PCM must already be copied, and the flush must cover the whole buffer. */
    assert(size == sizeof(pcm));
    assert(memcmp(address, pcm, size) == 0);
    flushed_address = address;
    flushed_bytes = size;
    return flush_result;
}
void ndspChnWaveBufAdd(int channel, ndspWaveBuf *wave)
{
    /* A fresh successful flush must precede every queue submission. */
    assert(initialized && flush_result == 0);
    assert(flushed_address == wave->data_vaddr && flushed_bytes == sizeof(pcm));
    assert(wave->status == NDSP_WBUF_FREE || wave->status == NDSP_WBUF_DONE);
    flushed_address = NULL;
    last_wave = wave;
    ++add_calls;
    wave->status = NDSP_WBUF_QUEUED;
    for (unsigned i = 0; i < DEF_SPEAKER_MAX_BUFFERS; ++i) {
        if (!queued[channel][i] || queued[channel][i] == wave) {
            queued[channel][i] = wave;
            return;
        }
    }
    assert(!"mock queue full");
}

static void test_handoff_and_reserve(void)
{
    assert(Util_speaker_add_buffer(0, pcm, sizeof(pcm)) == DEF_ERR_NOT_INITIALIZED);
    assert(Util_speaker_init() == DEF_SUCCESS);
    assert(Util_speaker_init() == DEF_ERR_ALREADY_INITIALIZED);
    assert(Util_speaker_add_buffer(0, pcm, sizeof(pcm)) == DEF_ERR_NOT_INITIALIZED);
    assert(Util_speaker_set_audio_info(0, 2, 48000) == DEF_SUCCESS);
    assert(Util_speaker_add_buffer(0, pcm, sizeof(pcm)) == DEF_SUCCESS);
    assert(flush_calls == 1 && add_calls == 1 && last_wave->nsamples == 2);
    assert(Util_speaker_get_available_buffer_size(0) == sizeof(pcm));
    assert(Util_speaker_get_available_buffer_num(0) == 1);

    last_wave->status = NDSP_WBUF_PLAYING;
    sample_position = 1;
    assert(Util_speaker_get_available_buffer_size(0) == 4);
    sample_position = 2;
    assert(Util_speaker_get_available_buffer_size(0) == 0);
    sample_position = 100; /* DSP advanced after the status read. */
    assert(Util_speaker_get_available_buffer_size(0) == 0);
    last_wave->status = NDSP_WBUF_DONE;
    assert(Util_speaker_add_buffer(0, pcm, sizeof(pcm)) == DEF_SUCCESS);
    assert(flush_calls == 2 && add_calls == 2);
    Util_speaker_clear_buffer(0);
    assert(Util_speaker_get_available_buffer_num(0) == 0);

    assert(Util_speaker_set_audio_info(0, 1, 32000) == DEF_SUCCESS);
    assert(Util_speaker_add_buffer(0, pcm, sizeof(pcm)) == DEF_SUCCESS);
    assert(last_wave->nsamples == 4);
    assert(Util_speaker_get_available_buffer_size(0) == sizeof(pcm));
    Util_speaker_exit();
    assert(!initialized);
}

static void test_rejected_buffers(void)
{
    assert(Util_speaker_init() == DEF_SUCCESS);
    assert(Util_speaker_set_audio_info(0, 2, 48000) == DEF_SUCCESS);
    unsigned allocations_before = alloc_calls;
    assert(Util_speaker_add_buffer(24, pcm, sizeof(pcm)) == DEF_ERR_INVALID_ARG);
    assert(Util_speaker_add_buffer(0, NULL, sizeof(pcm)) == DEF_ERR_INVALID_ARG);
    assert(Util_speaker_add_buffer(0, pcm, 0) == DEF_ERR_INVALID_ARG);
    assert(Util_speaker_add_buffer(0, pcm, 3) == DEF_ERR_INVALID_ARG);
    assert(Util_speaker_add_buffer(0, pcm, 6) == DEF_ERR_INVALID_ARG);
    assert(alloc_calls == allocations_before);

    unsigned flushes_before = flush_calls, adds_before = add_calls;
    allocation_fails = true;
    assert(Util_speaker_add_buffer(0, pcm, sizeof(pcm)) == DEF_ERR_OUT_OF_LINEAR_MEMORY);
    assert(flush_calls == flushes_before && add_calls == adds_before);
    allocation_fails = false;

    flush_result = (Result)-1;
    assert(Util_speaker_add_buffer(0, pcm, sizeof(pcm)) == UINT32_MAX);
    assert(flush_calls == flushes_before + 1 && add_calls == adds_before);
    assert(Util_speaker_get_available_buffer_num(0) == 0);
    flush_result = 0;
    assert(Util_speaker_add_buffer(0, pcm, sizeof(pcm)) == DEF_SUCCESS);
    assert(add_calls == adds_before + 1);
    Util_speaker_exit();
}

static void test_full_queue_and_relaunch(void)
{
    for (unsigned run = 0; run < 2; ++run) {
        assert(Util_speaker_init() == DEF_SUCCESS);
        assert(Util_speaker_set_audio_info(0, 2, 48000) == DEF_SUCCESS);
        for (unsigned i = 0; i < DEF_SPEAKER_MAX_BUFFERS; ++i)
            assert(Util_speaker_add_buffer(0, pcm, sizeof(pcm)) == DEF_SUCCESS);
        unsigned allocations_before = alloc_calls, flushes_before = flush_calls;
        assert(Util_speaker_add_buffer(0, pcm, sizeof(pcm)) == DEF_ERR_TRY_AGAIN);
        assert(alloc_calls == allocations_before && flush_calls == flushes_before);
        assert(Util_speaker_get_available_buffer_num(0) == DEF_SPEAKER_MAX_BUFFERS);
        Util_speaker_exit();
    }
}

int main(void)
{
    test_handoff_and_reserve();
    test_rejected_buffers();
    test_full_queue_and_relaunch();
    puts("speaker tests passed (mock NDSP handoff, not hardware timing)");
    return 0;
}
