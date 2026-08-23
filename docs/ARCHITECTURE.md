# Architecture

RetroTuner3DS combines a small live-stream front end with the proven decoder and
renderer from Video player for 3DS.

## Control path

`live_app.c` owns the channel deck, reads the user's M3U file, starts a network
worker, and hands a ready stream to the player. It also detects the player's
return and tears down the live session before another channel is selected.

## HLS path

1. `playlist.c` parses at most 32 M3U entries with bounded names and URLs.
2. `network.c` performs bounded HTTP(S) requests with TLS verification,
   redirects, timeouts, and response-size caps.
3. `hls.c` parses master and media playlists and resolves relative URLs.
4. `live_stream.c` selects the lowest advertised rendition and downloads live
   MPEG-TS segments on a producer thread. Initial tuning passes its already
   parsed media playlist to that producer, avoiding an immediate duplicate
   manifest request.
5. Each segment is first capped and staged in a 4 MiB ordinary-RAM buffer. Only
   a complete, validated MPEG-TS response is committed to playback.
6. A static 6 MiB BSS ring separates network timing from playback while
   avoiding scarce linear memory.
7. A custom FFmpeg input bridge exposes the ring as a streaming media source.

## Playback path

FFmpeg demuxes MPEG-TS and AAC. Compatible H.264 packets are normalized by
`h264_annexb.c` and submitted to the New 3DS MVD hardware decoder as complete
access units. The inherited player uploads decoded frames through the existing
Citro3D rendering path and uses the existing audio output path.

For a live source, the first decoded video frame establishes the video clock
before normal A/V catch-up dropping begins. This is necessary because MPEG-TS
audio and video can arrive with large absolute presentation timestamps; there
is no valid relative drift calculation until both playback clocks have a
baseline.

RetroTuner3DS scales decoded images for the 400x240 top display, but it does not
transcode the source. Decode cost therefore still depends on the original
resolution, profile, frame rate, and bitrate.

## Memory and failure boundaries

- Stream ring: 6 MiB in ordinary application BSS.
- Manifest and segment requests have fixed maximum sizes. The shared 4 MiB
  segment ceiling applies to both prefetch and live playback, and failures
  report received size, server length when known, and the cap without logging
  the channel URL.
- Live MVD input is restricted to one H.264/YUV420P track at no more than
  640x480 and, when reported, no more than 30.5 fps. Unsupported sources fail
  before `mvdstdInit`.
- Invalid physical buffers and fatal MVD results trigger serialized teardown.
  Render waits are bounded, and MVD-registered output surfaces remain allocated
  until the decoder service exits.
- The producer pauses at a high-water mark. Playback only enters its bounded
  three-second refill after the network ring actually runs empty.
- Encrypted, byte-range, and fMP4 playlists are rejected before handoff;
  later discontinuities or media-sequence gaps stop the producer before the
  changed segment reaches the decoder.
- Stop and channel-change paths request producer cancellation, join the thread,
  close FFmpeg/MVD resources, and reset the ring.

## Startup diagnostics

The tuning UI reports the current startup phase and elapsed time across root
manifest fetch, optional media-manifest selection, initial complete-segment
staging, FFmpeg probing, MVD initialization, and first-frame presentation.
These timings describe where startup latency occurred; they do not bypass the
whole-segment handoff, memory caps, codec preflight, or serialized teardown
boundaries above.

## Tests

Host tests exercise HLS parsing/staging and H.264 Annex B normalization under
AddressSanitizer and UndefinedBehaviorSanitizer. Console integration still
requires a real New 3DS-family device because MVD behavior cannot be faithfully
validated by ordinary desktop tests or current emulators.
