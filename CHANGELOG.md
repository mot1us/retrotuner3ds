# Changelog

All notable changes to RetroTuner3DS are recorded here. The project uses semantic
versioning while the player remains experimental.

## [Unreleased]

- Add sanitizer-backed M3U and HLS parser coverage, including 32-channel
  limits, quoted metadata, rendition selection, encryption tags, and URL
  resolution.
- Select HLS renditions by exact peak `BANDWIDTH` instead of accidentally
  treating `AVERAGE-BANDWIDTH` as the peak value.
- Build public release archives from a clean committed snapshot and refuse to
  package playlists, local media, or generated application binaries.
- Remove the obsolete embedded reference-clip entry point.
- Keep live hardware decode behind an H.264/YUV420P, 640x480, 30.5 fps
  preflight after two matching MVD service crash dumps; unsupported streams do
  not fall back to software video decoding.
- Restore the proven whole normalized H.264 access-unit submission path after
  rc9's experimental per-NAL parameter guard rejected ordinary broadcasts.
- Accept MVD's `0x17000` success status from `MVDSTD_SetConfig`; rc9 mistakenly
  surfaced that successful configuration as a fatal player error.
- Stage each HLS segment atomically so failed or truncated HTTP transfers never
  expose partial transport-stream data to FFmpeg/MVD.
- Use one 4 MiB ceiling for prefetch and live atomic segment staging, with
  URL-free received/content-length diagnostics when that cap is exceeded.
- Bound stalled MVD render waits, keep registered output surfaces alive until
  service exit, use the matching allocator for linear memory, and join every
  live worker before freeing shared state.
- Order START shutdown as producer cancellation, player-thread join, decoder
  close, then stream and network teardown.
- Stop cleanly at HLS discontinuities or media-sequence gaps.
- Fix the inherited 48-frame restart threshold that left the three-slot MVD
  queue permanently stuck at 4.17% on video-only or unsupported-audio feeds.
- Replace blank startup textures with a dark animated signal-lock screen.
- Keep channel changes in a dedicated current-to-next handoff view while the
  existing one-decoder teardown barrier runs.
- Relabel the meter as compressed `NETWORK RESERVE` and color it against the
  real refill target; decoded video and audio queues are downstream of it.
- Prepare the project for public GitHub development.
- Add repository documentation, issue forms, and host-test automation.
- Standardize the build output as `retrotuner3ds.3dsx`.
- Make `B` a latched, high-priority return-to-channels action during playing,
  pausing, buffering, and player error states.
- Route live-playback failures into the Pixel Deck status panel instead of the
  inherited modal error screen.
- Close the idle handoff race so a tuned channel starts from one `A` press.
- Show concise tuning errors with retry guidance on the channel deck.
- Validate one complete MPEG-TS segment before player handoff so tuning reaches
  a usable first frame instead of exposing a blank player screen.
- Replace the inherited status bar with a matching Pixel Deck signal strip.
- Center the RetroTuner3DS banner and color-code button labels separately from
  their actions for clearer controls.
- Estimate playable buffer time from measured segment bitrate and display
  buffer health in seconds instead of treating every stream alike.
- Restore the proven three-second recovery target and only block after a real
  empty-ring underrun; the proactive low-water pause caused healthy feeds to
  stall prematurely.
- Show source bitrate, measured network throughput, buffer time, download time,
  and real underrun counts for useful hardware diagnostics.
- Add animated tuning progress with elapsed time.
- Expand playlists to 32 stations with a ten-row paged channel deck.
- Add `L`/`R` live channel switching through the same full teardown path as
  returning to the deck, so two decoders or stream buffers never coexist.
- Remove bundled stations; users now supply
  `sd:/3ds/retrotuner3ds/channels.m3u` themselves.
- Rename the public project, application, and binary to RetroTuner3DS.

## [0.5.0] - 2026-08-22

### Added

- Retro Pixel Deck channel-selection interface.
- Six screened low-bitrate starter channels.
- On-device display of the selected rendition, buffer level, downloads,
  underruns, and errors.
- A compatibility indicator for streams near the New 3DS sweet spot.

### Changed

- Start tuning with one `A` press and request autoplay during player handoff.
- Keep the lower-screen dashboard available during playback.
- Hide inherited player controls until explicitly requested.
- Purge stream and decoder resources when leaving a channel.

## [0.4.0] - 2026-08-22

### Added

- Continuous HLS playback through a bounded 6 MiB ring buffer.
- Producer/consumer rebuffering with low- and high-water marks.
- New 3DS MVD H.264 hardware decoding through the established player pipeline.

### Verified

- TVS Turbo played continuously on real New 3DS hardware for several minutes,
  with only occasional brief rebuffering.

## [0.2.0] - 2026-08-22

- Added bounded HLS playlist parsing, segment staging, and sanitizer-backed
  host tests.

## [0.1.0] - 2026-08-22

- Proved networking, MPEG-TS demux, one-frame MVD decoding, and playback of an
  embedded reference clip on New 3DS hardware.

[Unreleased]: https://github.com/mot1us/retrotuner3ds/compare/v0.5.0...HEAD
[0.5.0]: https://github.com/mot1us/retrotuner3ds/releases/tag/v0.5.0
[0.4.0]: https://github.com/mot1us/retrotuner3ds/releases/tag/v0.4.0
[0.2.0]: https://github.com/mot1us/retrotuner3ds/releases/tag/v0.2.0
[0.1.0]: https://github.com/mot1us/retrotuner3ds/releases/tag/v0.1.0
