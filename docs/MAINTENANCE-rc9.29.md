# rc9.29 maintenance audit

September 11, 2026. Starting point: `697951f`, version `0.5.1-rc9.28`.
The user reports the current app works well. This pass keeps the streaming
design and UI intact; it is not a decoder rewrite or a buffer experiment.

## Upstream v1.8.0

[Video player for 3DS v1.8.0](https://github.com/Core-2-Extreme/Video_player_for_3DS/releases/tag/v1.8.0)
was published September 11 at commit
`3b326d272344087017549f5f1ac5eadf06e413b7`.
Our inherited snapshot is `9a0172351db2df3c06f52a3dfeb658c47cabddfd`,
from August 15. It already contained the release's dual-eye freeze fix,
oversized hardware-decoder guard, FTP code, and dependency updates.

The [nine newer commits](https://github.com/Core-2-Extreme/Video_player_for_3DS/compare/9a0172351db2df3c06f52a3dfeb658c47cabddfd...v1.8.0)
are mainly battery UI, colors, license screens, and updater work. There are no
new changes to `decoder.c`, `speaker.c`, or the bundled FFmpeg archives in that
comparison. No upstream code was imported for rc9.29.

Our local toolchain is already devkitARM r68-1 / GCC 16.1.0. FTP stays disabled;
our deliberately small FFmpeg build stays AAC/H.264-only. The release's new
subtitle and VVC support does not become RetroTuner support automatically.

## Changes made

- **Audio handoff:** flush the exact PCM buffer before NDSP submission, check
  the result, and release unsubmitted storage on failure. Reject partial PCM16
  sample frames before allocating.
- **Audio overhead:** skip `free(NULL)` calls when scanning the 512 audio slots.
  Our allocator wrapper takes a mutex even for null pointers. Allocation sizes,
  queue capacity, and the lifetime of real audio buffers stay the same.
- **Audio diagnostics:** clamp the remaining-sample calculation when the DSP
  advances between the status and position reads. This prevents an unsigned
  wrap from reporting an enormous reserve; it is still a non-atomic estimate.
- **Rendition selection:** share the existing manifest limits between the
  selector and scanner. Compatible candidates rank ahead of known-heavy ones,
  then bitrate decides. Unknown metadata remains provisional; heavy-only
  channels retain their existing rejection. Actual H.264 checks are unchanged.
- **Build cleanup:** default to `.3dsx`, fix explicit CIA target dependencies,
  and replace the inherited broad FFmpeg recipe with our recorded configuration.
  No library binaries or application license changed.

## Verification

The host suite now has nine executables, run with AddressSanitizer and
UndefinedBehaviorSanitizer. New regression cases exercise mixed HD/SD manifests,
frame-rate limits, missing metadata, PCM flush ordering, failure recovery, queue
exhaustion, remaining-sample bounds, and repeated speaker initialization/exit.
The NDSP functions in the audio test are mocks, not a hardware emulator.

Before handing off: run the full suite, public-tree hygiene check, and a fresh
devkitARM `.3dsx` build. Keep the old known-working binary for comparison.
No performance gain or audio-quality improvement is claimed without a device
comparison of the exact artifact.

For the device check, use the same playlist: play two familiar channels for
several minutes, switch with L/R and the channel drawer, cancel one tune, then
exit and relaunch. Listen for changes in audio and return the current/previous
telemetry logs if anything regresses. No full channel-by-channel retest is needed.

## Left for separate work

- Controlled HTTP/HLS fault tests for slow, truncated, and interrupted responses.
- Pinning CI's floating toolchain image after validating a chosen digest.
- Gradual extraction of the large inherited player into testable session logic.

Unused optional framework units are already excluded from compilation. Removing
their source files would not speed up playback, so this pass does not delete
whole subsystems or alter MVD ownership, A/V clocks, relock policy, or buffering.

## Hardware checkpoint

The subsequent New 3DS report says playback works well, with some buffering
and good audio. The on-card binary matches the delivered SHA-256:
`8486e782990f65ecc5d79a59f70465008f53256a69aa6188556bc0a181f3da3b`.

The current log contains 187 rc9.29 rows over 293 seconds. Three channels
reached playback with continued video presentation and no logged audio errors.
One recorded three compressed-ring underruns; the other two recorded none.
A fourth channel timed out after 30 seconds during its initial segment fetch.
This is a usable playback checkpoint, not a claim that every source works.

The older log identifies rc9.28. Device filesystem dates are unreliable, so
the embedded version and binary hash identify this run. These files do not
demonstrate exit/relaunch or a controlled before/after audio comparison.
Private raw logs are preserved outside Git at
`dist/hardware-rc9.29/telemetry-PPZ7UB/`.
