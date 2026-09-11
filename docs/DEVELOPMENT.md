# Development

RetroTuner3DS is normal C code until it reaches Nintendo-specific services.
Most parsing, buffering, and safety logic can be tested on a desktop. MVD,
Citro3D, audio timing, Wi-Fi behavior, and final teardown still need a real New
3DS.

## Open it in VS Code

Open `RetroTuner3DS.code-workspace`. Its terminal starts in the project root.
The checked-in tasks expect devkitPro at `/opt/devkitpro`; adjust the workspace
settings if your installation lives somewhere else.

Useful commands:

- **Terminal -> Run Build Task** builds `retrotuner3ds.3dsx`.
- **Terminal -> Run Task -> RetroTuner3DS: Host tests** runs the sanitizer tests.
- **Source Control** shows the same changes as `git status` and `git diff`.

The recommended C/C++ extension is optional. It helps with navigation, but the
devkitARM compiler is still the source of truth.

Normal builds link the checked-in libraries. The local toolchain is devkitARM
`r68-1` (GCC 16.1.0); changing it or a library is a separate dependency update,
not routine cleanup. The [FFmpeg rebuild notes](../library/ffmpeg_build.md)
describe the smaller configuration used here and its reproducibility limits.
CI currently uses `devkitpro/devkitarm:latest`, so its toolchain can drift from
the local one; record the actual compiler version when comparing build results.

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

## Remotes

Your fork is normally `origin`. If you want to compare inherited code, add the
original Video player for 3DS repository as a fetch-only `upstream` remote once:

```sh
git remote add upstream https://github.com/Core-2-Extreme/Video_player_for_3DS.git
git remote set-url --push upstream DISABLED
git fetch upstream --no-tags
git log --oneline --left-right main...upstream/main
```

## Public release packages

The packaging script exports a clean committed tree, runs the host tests, builds
the `.3dsx`, and refuses to include playlists. Replace `VERSION` with the value
in `include/miniiptv/version.h`:

```sh
./scripts/package-release.sh VERSION
```

That creates an explicitly untagged local test package. Before publishing, test
the exact commit on hardware, create an annotated `vVERSION` tag at that commit,
and require the tag during packaging:

```sh
git tag -a vVERSION -m "RetroTuner3DS VERSION"
git push origin HEAD vVERSION
RETROTUNER_REQUIRE_TAG=1 \
RETROTUNER_THIRD_PARTY_SOURCE_ARCHIVE=/path/to/audited-third-party-sources.zip \
    ./scripts/package-release.sh VERSION
```

Output goes to the ignored `dist/` directory with SHA-256 files, an unpacked
drop-in folder, and a project-source archive made from the exact release
commit. Tagged public packaging also refuses to run without a separately
audited source archive for the bundled third-party libraries. The pinned
revisions that belong in that archive are listed in `LICENSES/README.md`.

Public packages never contain `channels.m3u`. Keep private hardware playlists
and test packages outside the repository.
