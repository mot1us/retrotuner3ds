# Development

RetroTuner3DS is normal C code until it reaches Nintendo-specific services.
Most parsing, buffering, and safety logic can be tested on a desktop. MVD,
Citro3D, audio timing, Wi-Fi behavior, and final teardown still need a real New
3DS.

## Open it in VS Code

Open `RetroTuner3DS.code-workspace`. Its terminal starts in the project root
with the devkitPro environment used by this Mac.

Useful commands:

- **Terminal -> Run Build Task** builds `retrotuner3ds.3dsx`.
- **Terminal -> Run Task -> RetroTuner3DS: Host tests** runs the sanitizer tests.
- **Source Control** shows the same changes as `git status` and `git diff`.

The recommended C/C++ extension is optional. It helps with navigation, but the
devkitARM compiler is still the source of truth.

## A normal change

Start from the latest tested `main`:

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

Then commit and push the branch:

```sh
git add path/to/changed/files
git commit -m "fix: describe the change"
git push -u origin HEAD
```

Open a pull request into `main`. Playback changes should report desktop tests
and New 3DS results separately. It is fine to leave a pull request open while a
hardware build is being tested.

## Reading the diagnostics

The on-device pipeline page answers one useful question: where did progress
stop?

- `V packet>decode>texture>draw` follows video from FFmpeg to the screen.
- `A<tracks>:state demux>frame>queue` follows audio to the DSP queue.
- `P` is the producer state: `LIST`, `FETCH`, `EDGE`, `FULL`, or `ERROR`.
- `C:H` means a recent master selection was reused. The media playlist and
  initial segments were still downloaded fresh.

The buffer page shows delivery headroom, segment timing, jitter, desired
reserve, current live-edge lag, and underruns. These measurements now guide
session-only startup and refill decisions. They never change the fixed memory,
segment, codec, or decoder limits.

`telemetry.csv` records the same pipeline data plus memory headroom. The useful
video columns are `video_packets`, `video_decoded_frames`, `video_textures`, and
`video_presented_frames`. Audio has matching demux, frame, queue, and error
counters. A full network ring with a flat presentation count points downstream
of the network.

Player errors preserve their last video and audio snapshot. The snapshot is
cleared on the next tune so it cannot be confused with a different channel.

## Hardware reports

A useful test report includes:

- the exact build or commit;
- the channel name, but not private URLs or tokens;
- time to first frame;
- how long it played before a stall or failure;
- whether audio, video, or both stopped;
- the matching `telemetry.csv` and, after a crash, the Luma dump.

Testing the same URL in VLC is also helpful. It proves the source is currently
alive, but not that its codec, segment layout, or resolution is safe for MVD.

## Working with Codex

The easiest requests name the result, the latest hardware observation, and the
Git boundary. For example:

> Fix B so one press returns from buffering. Run the host tests and build a
> hardware package, but let me test it before anything reaches main.

You can review every edit in VS Code before it is committed. Useful checkpoints
are “show me the diff,” “commit but do not push,” and “this passed hardware;
merge it to main.”

## Remotes

- `origin` is `mot1us/retrotuner3ds`.
- `upstream` is the original Video player for 3DS repository and is fetch-only.

To inspect upstream without merging it:

```sh
git fetch upstream
git log --oneline --left-right main...upstream/main
```

## Public release packages

Only package a commit after its candidate has passed real hardware testing.
The script exports a clean committed tree, runs tests, builds the `.3dsx`, and
refuses to include playlists:

```sh
./scripts/package-release.sh 0.5.1-rc9.28
```

The argument must match `RETROTUNER_VERSION` in
`include/miniiptv/version.h`. Output goes to the ignored `dist/` directory with
a SHA-256 file and an unpacked drop-in folder.

Public packages never contain `channels.m3u`. Keep private hardware playlists
and test packages outside the repository.
