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
   manifest request. Four successful master selections are cached for 60
   seconds; a hit skips only the root request and still refreshes the media
   playlist before staging playback data.
5. Each segment is first capped and staged in a 4 MiB ordinary-RAM buffer. Only
   a complete, validated MPEG-TS response is committed to playback.
6. If the selected master declares a separate audio rendition, matching
   video/audio segments are aligned by `EXT-X-PROGRAM-DATE-TIME`. A bounded TS
   combiner adds the AAC elementary stream to the video program map and
   interleaves remapped audio packets. Layouts that cannot be combined safely
   are rejected before FFmpeg sees them.
7. A static 6 MiB BSS ring separates network timing from playback while
   avoiding scarce linear memory.
8. A custom FFmpeg input bridge exposes the ring as a streaming media source.

An observation-only shadow controller samples complete segment delivery and
actual ring underruns. It derives integer EWMAs for content rate, network
headroom, delivery gaps, and gap deviation, then reports a desired reserve and
live-edge lag. The controller owns no pointers and cannot change playback in
rc9.7; its state is discarded with the existing stream teardown.

The app/draw owner samples that snapshot, never the network or decoder hot
paths. A small CSV logger buffers 8 KiB in ordinary RAM, uses dense startup and
sparse steady-state sampling, flushes every ten seconds and on important
events, and stops at 512 KiB. The replace-on-launch file contains channel names
and measurements but no URLs or media payloads. Logging failure is non-fatal
and cannot alter stream state.

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
- Manifest and segment requests have fixed maximum sizes. Video and combined
  segments use a 4 MiB ceiling; a separate audio segment uses a 1 MiB staging
  ceiling plus a bounded 4 MiB combine scratch area in ordinary BSS. Failures
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
- Encrypted, byte-range, and fMP4 playlists are rejected before handoff.
  Explicit discontinuities and playlist regressions stop the producer before
  changed media reaches the decoder. An ordinary forward media-sequence gap is
  treated as a recoverable live-window resynchronization and logged.
- Stop and channel-change paths request producer cancellation, join the thread,
  close FFmpeg/MVD resources, and reset the ring.

## Startup diagnostics

The tuning UI reports the current startup phase and elapsed time across root
manifest fetch, optional media-manifest selection, initial complete-segment
staging, FFmpeg probing, MVD initialization, and first-frame presentation.
These timings describe where startup latency occurred; they do not bypass the
whole-segment handoff, memory caps, codec preflight, or serialized teardown
boundaries above.

During player startup, the live overlay also reports video packets accepted by
the decoder, MVD output frames, uploaded textures, actual draw completion,
audio demux/frame/output flow, and producer state. Player open, MVD init, and
first-frame phases have separate bounded waits. Their timeout path uses the
ordinary worker-owned abort sequence; it never frees MVD surfaces from the draw
thread.

After the first frame, `SELECT` cycles a shadow-buffer page, the pipeline page,
and a clean view. `SHADOW` values are recommendations for later hardware-tested
releases, not active settings.

## Tests

Host tests exercise HLS parsing/staging, bounded MPEG-TS audio/video program
combining, H.264 Annex B normalization, the integer shadow-buffer controller,
and the bounded CSV logger under AddressSanitizer and UndefinedBehaviorSanitizer.
Console integration still requires a real New 3DS-family device because MVD
behavior cannot be faithfully validated by ordinary desktop tests or current
emulators.
