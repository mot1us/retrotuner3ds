# RetroTuner3DS

RetroTuner3DS is a low-bitrate M3U/HLS stream player for the New Nintendo 3DS.
Add an M3U playlist and it scans for compatible channels.

This is still an experiment. It has only been tested on a New Nintendo 3DS,
and some streams will fail even after they pass the first scan. Live channels
change formats, disappear, and occasionally publish segments that are too much
for the console.

## What works best

The sweet spot is H.264 video with AAC audio at 360p and roughly 1 Mbps or
less. Some 480p streams work too. The app selects the lowest rendition a
channel offers, but it does not transcode HD streams.

RetroTuner3DS currently supports:

- live HLS using MPEG-TS segments;
- H.264/AVC video and AAC audio;
- up to 640x480 at a known 30.5 fps or lower;
- unencrypted streams without byte ranges or fMP4 init maps;
- playlists with up to 64 channels.

## Install

1. Put `retrotuner3ds.3dsx` at
   `sd:/3ds/retrotuner3ds/retrotuner3ds.3dsx`.
2. Put your playlist at `sd:/3ds/retrotuner3ds/channels.m3u`.
3. Open RetroTuner3DS from the Homebrew Launcher.

No channels or stream URLs are included. Bring your own M3U and make sure you
have permission to use the streams in it.

## Controls

| Control | Action |
| --- | --- |
| Up / Down | Pick a channel |
| Left / Right | Change channel-list page |
| A | Tune or retry |
| B | Cancel, go back, or open the channel drawer during playback |
| L / R | Previous or next channel |
| Select | Cycle the diagnostic screens |
| Start | Exit |

Channels appear as the opening scan finds them. A `*` means the stream openly
advertised compatible video details. A `?` means the app needs to tune it
before it can know for sure. During that session, `+` means a channel played
and `!` means it failed to start. Nothing is permanently blocked; you can
always try again.

## Playlist example

```m3u
#EXTM3U
#EXTINF:-1,Example Channel
https://example.test/live/index.m3u8
```

Extended-M3U `user-agent` and `referrer` attributes are supported when a
legitimate stream requires them.

## What the app is doing

```text
M3U playlist -> quick manifest scan -> channel list
                                      |
HLS playlist -> lowest safe rendition -> network buffer
                                      |
                         FFmpeg + AAC + MVD H.264 -> Citro3D
```

The network thread downloads complete HLS segments into a fixed 6 MiB ring
while the player reads from the other side. A new channel normally starts with
two complete segments in reserve. If the same channel is tuned again, the app
can use what it learned earlier in that session to choose a faster or safer
starting point.

The opening scan is deliberately shallow. It reads manifests, not video, so it
can find stations quickly. The real codec, resolution, segment-size, and MVD
checks happen when you tune. That is why a channel can appear in the list and
still fail safely afterward.

The full technical versions live in [Architecture](docs/ARCHITECTURE.md) and
[Buffering](docs/ADAPTIVE_BUFFERING.md).

## Telemetry

The app writes diagnostic data to:

```text
sd:/3ds/retrotuner3ds/telemetry.csv
```

The previous run is kept as `telemetry-prev.csv`. The logs contain timing,
buffer, decoder, audio, relock, underrun, and error data. They do not contain
stream URLs, video, or audio. Logging stops at 512 KiB and is never required
for playback.

## Building it

Install the devkitPro 3DS toolchain, then run:

```sh
make 3dsx -j4
./tests/run_live_host_tests.sh
```

More setup and release notes are in the [development guide](docs/DEVELOPMENT.md).
You can also read [Contributing](CONTRIBUTING.md), the
[Changelog](CHANGELOG.md), and the [Roadmap](ROADMAP.md).

## Safety, credits, and license

RetroTuner3DS is a `.3dsx` homebrew app. It does not touch NAND, firmware,
boot configuration, Luma configuration, or the title database.

The project is built on
[Video player for 3DS](https://github.com/Core-2-Extreme/Video_player_for_3DS)
by Core_2_Extreme and is released under **GPL-3.0-or-later**. See
[LICENSE](LICENSE), [Third-party notices](THIRD_PARTY_NOTICES.md), and the
[bundled dependency licenses](LICENSES/README.md).
