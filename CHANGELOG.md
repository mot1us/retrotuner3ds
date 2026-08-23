# Changelog

All notable changes to RetroTuner3DS are recorded here. The project uses semantic
versioning while the player remains experimental.

## [Unreleased]

- Prepare the project for public GitHub development.
- Add repository documentation, issue forms, and host-test automation.
- Standardize the build output as `retrotuner3ds.3dsx`.
- Make `B` a latched, high-priority return-to-channels action during playing,
  pausing, buffering, and player error states.
- Route live-playback failures into the Pixel Deck status panel instead of the
  inherited modal error screen.
- Close the idle handoff race so a tuned channel starts from one `A` press.
- Show concise tuning errors with retry guidance on the channel deck.
- Begin playback after one validated HLS segment while the producer continues
  filling the bounded ring buffer in parallel.
- Replace the inherited status bar with a matching Pixel Deck signal strip.
- Center the RetroTuner3DS banner and color-code button labels separately from
  their actions for clearer controls.
- Estimate playable buffer time from measured segment bitrate, display buffer
  health in seconds, and scale rebuffer recovery to roughly three seconds of
  each stream instead of a fixed byte count.
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
