# Development guide

## Open in VS Code

Open `RetroTuner3DS.code-workspace`. The workspace starts terminals in the project
root and supplies the devkitPro environment used by this Mac.

Useful VS Code commands:

- **Terminal → New Terminal** opens a shell ready for Git and devkitARM.
- **Terminal → Run Build Task** builds `retrotuner3ds.3dsx`.
- **Terminal → Run Task → RetroTuner3DS: Host tests** runs the sanitizer suite.
- **Source Control** shows the same staged/unstaged changes as `git status`.

The recommended C/C++ extension is optional; it provides navigation and
diagnostics but does not replace the devkitARM compiler.

## Daily Git flow

Start new work from the latest tested `main`:

```sh
git switch main
git pull --ff-only
git switch -c fix/short-description
```

While working:

```sh
git status
git diff
./tests/run_live_host_tests.sh
make 3dsx -j4
```

Commit a focused result:

```sh
git add path/to/changed/files
git commit -m "fix: describe the behavior"
git push -u origin HEAD
```

Open a pull request into `main`. For playback work, record desktop test results
and the real-hardware result separately. A pull request can remain open while a
`.3dsx` candidate is being tested.

## Live diagnostics

The pipeline detail page uses compact counters:

- `V packet>decode>texture>draw` identifies the first video stage that stopped.
- `A<tracks>:state demux>frame>queue` separates FFmpeg audio discovery,
  decoding, and DSP output.
- `P` is the producer state (`LIST`, `FETCH`, `EDGE`, `FULL`, or `ERROR`).
- `C:H` means the 60-second rendition cache skipped the root master request;
  the media playlist and complete initial segment were still downloaded.

These counters are diagnostic only and do not relax the MVD, segment-size,
whole-segment, or teardown safety boundaries.

During playback, `telemetry.csv` also records memory headroom every five
seconds. `heap_used_bytes/heap_total_bytes` covers the fixed ordinary heap;
`linear_free_bytes/linear_total_bytes` covers the larger linear allocator used
by video, FFmpeg, and larger wrapped allocations; `app_region_total_bytes`
identifies the process memory layout. The sampler reads allocator counters and
never allocates test blocks.

The CSV's `video_packets`, `video_decoded_frames`, `video_textures`, and
`video_presented_frames` columns locate a downstream video stall. Audio uses
`audio_tracks`, `audio_state`, `audio_demux_packets`, `audio_frames`,
`audio_buffers`, and `audio_last_error`. These are existing atomic counters;
logging them adds no decoder-thread work.

The rc9.7 `SHADOW` page reports what a future adaptive-buffer controller would
choose while leaving the actual player unchanged:

- `H` is segment media time divided by download time (`2.0x` means the network
  delivered that sample twice as fast as playback consumes it).
- `WANT` is the calculated reserve target and `LAG` is the suggested number of
  published HLS segments to stay behind the live edge.
- `SEG`, `GAP`, and `JIT` are smoothed media duration, complete-segment delivery
  gap, and delivery-gap deviation.
- `U90` counts real empty-ring underruns in the recent 90-second window.

Press `SELECT` to cycle shadow telemetry, pipeline diagnostics, and a clean
view. Every value is numeric telemetry copied under the stream lock; it does
not change the rc9.7 tune, ring, or decoder behavior.

Player failures return to a dedicated error panel that preserves the final
video and audio snapshot on separate readable rows. This snapshot is cleared
when a new tune begins, so it cannot be mistaken for the next station.

Live startup may leave one validated texture queued while the inherited player
is in `BUFFERING`. Because the two-slot texture ring has one usable pending
slot, waiting for the normal two-frame refill threshold at that point would
deadlock presentation. The live-only startup path therefore uses the existing
buffering-complete notification when that first texture is ready. Drawing it
restores the normal refill policy; no decoder or memory limits are changed.

## Working with Codex

A productive request usually names the outcome and supplies the latest hardware
observation, for example:

> Fix B so one press returns from playback during buffering. Run host tests and
> build a hardware-test package, but do not merge until I test it.

Codex can inspect and edit the same working tree, run tests, prepare focused
commits, and summarize the exact hardware checks needed. You can review every
change in VS Code's Source Control view before it is committed or pushed.

Useful checkpoints to request are:

- “Show me the diff before committing.”
- “Commit this fix but do not push it.”
- “Push this branch and prepare a pull request.”
- “Tag the hardware-tested build and prepare a release.”

## Remotes

- `origin` is the public `mot1us/retrotuner3ds` repository.
- `upstream` is the original Video player for 3DS repository and is configured
  fetch-only to avoid accidental pushes.

To inspect upstream changes without merging them:

```sh
git fetch upstream
git log --oneline --left-right main...upstream/main
```

## Public release package

Only package a commit after its release candidate has passed real New 3DS
hardware testing. The packaging script exports the committed Git tree into a
temporary directory, tests and builds that clean snapshot, and refuses to
include playlists:

```sh
./scripts/package-release.sh 0.5.1-rc9.16
```

The argument must exactly match `RETROTUNER_VERSION` in
`include/miniiptv/version.h`; update that one definition before preparing a
different candidate or final release.

The ZIP, its SHA-256 file, and the unpacked drop-in folder are written under
the ignored `dist/` directory. Public packages never include `channels.m3u`;
each user supplies that file on their own SD card. Private hardware-test
packages belong outside the repository and must not be attached to a public
release.
