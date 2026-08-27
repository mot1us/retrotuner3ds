# Architecture

This page is the technical tour of RetroTuner3DS: where a channel goes after
you press `A`, how the buffer is kept inside a hard memory limit, and where the
player decides to give up instead of risking the console.

## The short version

```text
channels.m3u
    -> manifest-only scan
    -> discovered channel list
    -> HLS rendition selection
    -> complete MPEG-TS segments
    -> 6 MiB compressed-data ring
    -> FFmpeg demux and AAC decode
    -> New 3DS MVD H.264 decode
    -> Citro3D top-screen output
```

The network producer and media player run separately. The ring between them
absorbs ordinary Wi-Fi and HLS timing swings without letting a stream consume
unbounded memory.

## Finding channels

`live_app.c` owns two lists: everything parsed from the user's M3U and the
smaller list shown on screen. The scanner checks channels one at a time and
adds anything that is clearly compatible or cannot yet be ruled out.

The scan only downloads manifests. It does not fetch video, open FFmpeg, or
start MVD. That keeps startup quick, but it also means a `?` channel can still
fail when the first real segment is inspected.

Scanning stops before a tune begins and resumes after playback returns to the
deck. This gives the active channel the New 3DS Wi-Fi connection and keeps one
clear owner for the persistent curl handle.

## Tuning and HLS

The path is split across a few small pieces:

1. `playlist.c` reads up to 64 bounded M3U entries.
2. `network.c` handles HTTP(S), TLS verification, redirects, cancellation,
   timeouts, and response-size limits.
3. `hls.c` parses master and media playlists and resolves relative URLs.
4. `channel_scan.c` performs the cheap manifest-only compatibility check.
5. `live_stream.c` selects the lowest advertised rendition and starts the
   segment producer.

A successful master selection can be cached for 60 seconds. A cache hit skips
one root-manifest request, but the media playlist is always refreshed before
playback starts.

Every media segment is downloaded into a 4 MiB staging buffer first. It enters
the ring only after the request finishes and the MPEG-TS data passes validation.
FFmpeg never sees a partial download.

Some HLS feeds publish video and AAC audio as separate MPEG-TS renditions. If
their program times line up, `ts_mux.c` combines them into one bounded transport
stream. If they cannot be matched safely, the channel is rejected.

## Playback

FFmpeg reads the ring through a custom streaming input bridge. It demuxes the
transport stream and decodes AAC audio. H.264 packets are normalized into
complete Annex B access units by `h264_annexb.c`, then sent to Nintendo's MVD
hardware decoder.

The first presented video frame becomes the live A/V clock baseline. Broadcast
streams often arrive with large, unrelated absolute timestamps; treating those
as normal relative playback time can make the player drop every frame after
the first one.

Decoded video is drawn through the inherited Citro3D renderer. Scaling a frame
to 400x240 only changes how it is displayed. It does not make a 720p or 1080p
source cheaper to decode, which is why the source limits matter.

## Buffering

The compressed-data ring is a static 6 MiB block in ordinary application
memory. It is not linear video memory and it never grows.

A cold tune normally stages two complete segments. The app measures segment
duration, download time, delivery gaps, jitter, and actual empty-ring
underruns. Up to 32 channel profiles are kept in memory for the current app
session. On a later tune, a healthy channel may use a faster one-segment start;
a marginal channel can begin farther behind the live edge.

If playback truly empties the ring, recovery waits for one complete segment.
A repeated underrun within 90 seconds asks for a deeper reserve, but the wait
and target are both bounded. More detail is in [Buffering](ADAPTIVE_BUFFERING.md).

## Format changes and safe failure

Live feeds are not as fixed as local files. A broadcaster can skip forward,
insert a discontinuity, change H.264 parameters, or replace the stream while
the app is already decoding it.

RetroTuner3DS does not feed changed media into the active MVD session. Sequence
gaps, playlist regressions, explicit discontinuities, and unsafe H.264 parameter
changes end the current decoder session. The app may make up to two clean relock
attempts after FFmpeg and MVD are fully torn down.

An established stream may skip at most two oversized segments. A third is a
hard failure. These limits are intentionally conservative; returning to the
channel deck is better than reusing the decoder in an unknown state.

## Memory and ownership rules

- Compressed ring: 6 MiB in ordinary BSS.
- Video or combined segment staging: 4 MiB maximum.
- Separate audio segment staging: 1 MiB maximum.
- Combined A/V scratch space: 4 MiB in ordinary BSS.
- Video: one H.264/YUV420P track, at most 640x480 and a known 30.5 fps.
- One producer, one FFmpeg consumer, and one MVD instance at a time.

Stopping or changing a channel always follows the same order: cancel the
producer, join its thread, close FFmpeg and MVD, then reset the ring. MVD output
surfaces stay allocated until the decoder service has actually exited.

## Telemetry

The UI reads copied snapshots rather than touching the network or decoder hot
paths. `telemetry_log.c` buffers a small CSV in ordinary RAM, writes frequently
during startup, slows down during steady playback, and stops at 512 KiB.

The log includes channel names and numeric measurements, but never URLs or
media. A logging failure is non-fatal. The previous run is rotated to
`telemetry-prev.csv` so one accidental relaunch does not erase the useful test.

## Tests

Desktop sanitizer tests cover playlist and HLS parsing, URL resolution, MPEG-TS
combining, H.264 normalization, buffer decisions, network limits, and telemetry.
The final playback path still needs a real New 3DS because ordinary desktop
tests and current emulators cannot reproduce Nintendo's MVD behavior.
