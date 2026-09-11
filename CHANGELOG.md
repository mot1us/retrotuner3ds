# Changelog

This is the useful summary of what changed. The individual release-candidate
experiments and hardware-test fixes are still available in the Git history.

## 0.5.1-rc9.31 - theme saving

- Replace temporary-file renames with checked writes and read-back verification.
  Keep the previous valid theme in `theme.cfg.bak` and use it if the main
  preference cannot be read. Saving an unchanged theme skips the rewrite.
- Show the failing save step and error code. `theme.cfg.log` keeps the last
  attempt, without adding settings writes to the playback loop.
- Add failure-injection tests for settings writes and recovery.
- Shorten the README and correct the playback controls.

The theme visuals were tested in rc9.30. This persistence fix still needs
an on-device save/restart check. Streaming and playback are unchanged.

## 0.5.1-rc9.30 - themes

- Added Classic, Cyberpunk, Wasteland, and Old-Time Radio themes, shared by
  startup, the channel deck, tuning, playback controls, and diagnostics.
- X opens a bottom-screen picker without stopping video. D-pad previews,
  A applies/saves, and B/X cancels the preview. START still exits.
- Saved preferences are bounded and optional; malformed files fall back to
  Classic. Hardware testing found a save warning, addressed in rc9.31.
- Added static neon/terminal/radio accents and themed tuning noise, using
  existing draw primitives. No images, extra workers, or video tinting.
- Added palette contrast/preference tests and mocked theme-picker tests.

Playback, buffering, decoder/audio handoff, and channel selection behavior
remain the tested rc9.29 baseline. Theme visuals worked on hardware;
the save path needed the follow-up above.

## 0.5.1-rc9.29 - 2026-09-11 (hardware test build)

- Reviewed Video player for 3DS v1.8.0. Its relevant decoder fixes were already
  in our starting snapshot; no upstream merge or dependency upgrade was needed.
- Flush copied PCM data before queuing it for the audio hardware. A failed
  flush is returned as an error instead of submitting unflushed data.
- Avoid allocator locks for empty audio slots and prevent audio-reserve
  arithmetic from wrapping when the DSP advances between reads.
- Prefer an HLS rendition within the existing resolution/frame-rate limits
  before comparing bitrate. A cheap HD rendition no longer hides an SD option.
- Add mocked audio-handoff tests and more rendition-selection regression tests.
- Make plain `make` build the supported `.3dsx` only. Explicit CIA targets now
  wait for their executable instead of racing it in parallel builds.
- Correct the FFmpeg rebuild recipe to match our slim bundled configuration.

Buffer sizes, startup policy, MVD safety checks, and UI are unchanged.
See the [maintenance audit](docs/MAINTENANCE-rc9.29.md) for scope and testing.

## Earlier 0.5.1 development

### Live TV

- Added continuous HLS playback through a fixed 6 MiB compressed-data ring.
- Added an opening manifest scan that fills the channel list as compatible or
  still-unknown stations are found.
- Raised the user playlist limit to 64 channels and removed all bundled
  stations from public builds.
- Selected the lowest advertised HLS rendition and reused recent master
  selections without reusing stale media playlists.
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

- Kept every segment atomic: incomplete or oversized HTTP responses never
  reach FFmpeg or MVD.
- Rejected unsupported codecs, layouts, resolutions above 640x480, and known
  frame rates above 30.5 fps before hardware decode.
- Validated live H.264 parameter sets and failed closed when they changed or
  became malformed.
- Treated HLS discontinuities, sequence gaps, playlist regressions, and
  detected format changes as decoder boundaries, fully tearing down FFmpeg/MVD
  before a bounded clean relock.
- Bounded manifest requests, player setup, MVD initialization, first-frame
  waits, render waits, retries, and the complete initial tune.
- Fixed several teardown and channel-handoff races that could leave a worker,
  stream buffer, or MVD surface alive too long.
- Serialized whole stream start/stop operations, synchronized the MVD output
  queue, and made abnormal decoder timeouts exit without freeing live state.
- Removed the abandoned SD-card cached-clip path and the obsolete embedded
  reference clip.

### Project

- Added sanitizer-backed host tests for M3U/HLS parsing, URL resolution,
  MPEG-TS combining, H.264 normalization, buffering decisions, network
  cancellation cleanup, and telemetry.
- Added clean-tree release packaging with checksums and checks for known
  playlist, media, and generated-binary file types.
- Added a devkitARM target build to CI so console-only code is compiled for
  `main` and pull requests.
- Added pinned third-party source revisions, retained license texts, development
  notes, and hardware-report templates.
- Removed inherited update, usage-report, and connectivity requests plus unused
  FTP, capture, recorder, encoder, and legacy network build units.

## 0.5.0 development milestone - 2026-08-22

- Added the first Retro Pixel Deck channel interface.
- Added live buffer and stream diagnostics.
- Started tearing down stream and decoder state when leaving a channel.

## 0.4.0 development milestone - 2026-08-22

- Added continuous HLS playback through the first bounded ring-buffer design.
- Connected live H.264 to the New 3DS MVD hardware decoder.
- Played TVS Turbo for several minutes on real hardware with brief buffering.

## 0.2.0 development milestone - 2026-08-22

- Added bounded HLS parsing, segment staging, and the first host tests.

## 0.1.0 development milestone - 2026-08-22

- Proved networking, MPEG-TS demux, one-frame MVD decode, and playback of a
  local reference clip on a New 3DS.
