# Changelog

This is the useful summary of what changed. The individual release-candidate
experiments and hardware-test fixes are still available in the Git history.

## [Unreleased]

### Live TV

- Added continuous HLS playback through a fixed 6 MiB compressed-data ring.
- Added an opening manifest scan that fills the channel list as compatible or
  still-unknown stations are found.
- Raised the user playlist limit to 64 channels and removed all bundled
  stations from public builds.
- Select the lowest advertised HLS rendition and reuse recent master selections
  without reusing stale media playlists.
- Added support for aligned HLS feeds that publish H.264 video and AAC audio in
  separate MPEG-TS renditions.

### Buffering and playback

- Cold channels now start with two complete segments. Healthy repeat tunes can
  use a faster session-only profile, while marginal channels can begin farther
  behind the live edge.
- Added bounded refill behavior for real empty-ring underruns and deeper
  recovery after repeated stalls.
- Added a fixed 24-second network-reserve display, adaptive target marker, ring
  occupancy, and a small PCM activity meter.
- Established the first presented live frame as the A/V clock baseline. This
  fixes streams whose audio and video arrive with unrelated absolute MPEG-TS
  timestamps.
- Kept queued audio running through brief video-only refills when compressed
  network data is still healthy.

### Channel deck and controls

- Reworked the interface into a simpler 1990s handheld-TV style with animated
  static, a clearer tune sequence, readable errors, and cleaner playback.
- Added a live channel drawer. Press `B` to browse on the bottom screen while
  the current station keeps playing above.
- Made `B`, `L`, and `R` work during tuning, buffering, playback, and errors
  without starting overlapping decoder sessions.
- Separated browsing from the active tune: moving the cursor does not switch
  channels until `A` confirms it.
- Blocked inherited D-pad brightness shortcuts inside RetroTuner and disabled
  idle sleep while the app is open.

### Diagnostics

- Added `telemetry.csv` with buffer, network, video, audio, memory, relock,
  underrun, and error data. The previous launch is kept separately.
- Added readable on-device pipeline and buffering pages while keeping the
  normal player view clean.
- Added truthful weighted tune progress so long segment downloads no longer
  look like a frozen four-step animation.
- Made player failures preserve the last video/audio snapshot and the original
  producer error.

### Safety and stability

- Keep every segment atomic: incomplete or oversized HTTP responses never
  reach FFmpeg or MVD.
- Reject unsupported codecs, layouts, resolutions above 640x480, and known
  frame rates above 30.5 fps before hardware decode.
- Validate live H.264 parameter sets and fail closed when they change or become
  malformed.
- Treat HLS discontinuities, sequence gaps, playlist regressions, and detected
  format changes as decoder boundaries. Recovery fully tears down FFmpeg/MVD
  before a bounded clean relock.
- Bound manifest requests, player setup, MVD initialization, first-frame waits,
  render waits, retries, and the complete initial tune.
- Fixed several teardown and channel-handoff races that could leave a worker,
  stream buffer, or MVD surface alive too long.
- Removed the abandoned SD-card cached-clip path and the obsolete embedded
  reference clip.

### Project

- Added sanitizer-backed host tests for M3U/HLS parsing, URL resolution,
  MPEG-TS combining, H.264 normalization, buffering decisions, network limits,
  and telemetry.
- Added clean-tree release packaging with checksums and hard checks that prevent
  playlists, stream URLs, media, and generated binaries from entering public
  source or release packages.
- Added pinned third-party source revisions, retained license texts, development
  notes, and hardware-report templates.

## [0.5.0] - 2026-08-22

- Added the first Retro Pixel Deck channel interface.
- Added live buffer and stream diagnostics.
- Started tearing down stream and decoder state when leaving a channel.

## [0.4.0] - 2026-08-22

- Added continuous HLS playback through the first bounded ring-buffer design.
- Connected live H.264 to the New 3DS MVD hardware decoder.
- Played TVS Turbo for several minutes on real hardware with brief buffering.

## [0.2.0] - 2026-08-22

- Added bounded HLS parsing, segment staging, and the first host tests.

## [0.1.0] - 2026-08-22

- Proved networking, MPEG-TS demux, one-frame MVD decode, and playback of a
  local reference clip on a New 3DS.

[Unreleased]: https://github.com/mot1us/retrotuner3ds/compare/v0.5.0...HEAD
[0.5.0]: https://github.com/mot1us/retrotuner3ds/releases/tag/v0.5.0
[0.4.0]: https://github.com/mot1us/retrotuner3ds/releases/tag/v0.4.0
[0.2.0]: https://github.com/mot1us/retrotuner3ds/releases/tag/v0.2.0
[0.1.0]: https://github.com/mot1us/retrotuner3ds/releases/tag/v0.1.0
