# Architecture

RetroTuner3DS combines a small live-stream front end with the proven decoder and
renderer from Video player for 3DS.

## Control path

`live_app.c` owns the channel deck, reads the user's M3U file, starts a network
worker, and hands a ready stream to the player. It also detects the player's
return and tears down the live session before another channel is selected.

## HLS path

1. `playlist.c` parses at most ten M3U entries with bounded names and URLs.
2. `network.c` performs bounded HTTP(S) requests with TLS verification,
   redirects, timeouts, and response-size caps.
3. `hls.c` parses master and media playlists and resolves relative URLs.
4. `live_stream.c` selects the lowest advertised rendition and downloads live
   MPEG-TS segments on a producer thread.
5. A static 6 MiB BSS ring buffer separates network timing from playback while
   avoiding scarce linear memory.
6. A custom FFmpeg input bridge exposes the ring as a streaming media source.

## Playback path

FFmpeg demuxes MPEG-TS and AAC. Compatible H.264 access units are normalized by
`h264_annexb.c` and submitted to the New 3DS MVD hardware decoder. The inherited
player uploads decoded frames through the existing Citro3D rendering path and
uses the existing audio output path.

RetroTuner3DS scales decoded images for the 400x240 top display, but it does not
transcode the source. Decode cost therefore still depends on the original
resolution, profile, frame rate, and bitrate.

## Memory and failure boundaries

- Stream ring: 6 MiB in ordinary application BSS.
- Manifest and segment requests have fixed maximum sizes.
- The producer pauses at a high-water mark and playback rebuffers at a low-water
  mark.
- Unsupported encrypted, byte-range, discontinuous, or fMP4 playlists are
  rejected before player handoff.
- Stop and channel-change paths request producer cancellation, join the thread,
  close FFmpeg/MVD resources, and reset the ring.

## Tests

Host tests exercise HLS parsing/staging and H.264 Annex B normalization under
AddressSanitizer and UndefinedBehaviorSanitizer. Console integration still
requires a real New 3DS-family device because MVD behavior cannot be faithfully
validated by ordinary desktop tests or current emulators.
