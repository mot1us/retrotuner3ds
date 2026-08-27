# RetroTuner3DS

An experimental low-bitrate M3U/HLS player for the **New Nintendo 3DS**.
It has only been tested on that model; other 3DS-family systems are unverified.

> [!IMPORTANT]
> RetroTuner3DS is early homebrew. It works best with low-bitrate H.264/AAC
> streams at 480p or below. Streams can change or disappear, so an entry that
> passes the initial scan may still fail during playback.

## Features

- Continuous HLS playback through a bounded 6 MiB compressed-data ring.
- New 3DS hardware-accelerated H.264 decoding.
- Fast compatibility scan for up to 64 user-provided channels.
- Automatic selection of the lowest advertised HLS rendition.
- Retro dual-screen channel browser and live channel switching.
- Bounded hardware telemetry for diagnosing buffering and stream failures.

The most reliable hardware-tested sources are 360p-class H.264/AAC MPEG-TS
streams below roughly 1 Mbps. RetroTuner3DS does not transcode video.

## Install

1. Copy `retrotuner3ds.3dsx` to:
   `sd:/3ds/retrotuner3ds/retrotuner3ds.3dsx`
2. Add your playlist at:
   `sd:/3ds/retrotuner3ds/channels.m3u`
3. Launch RetroTuner3DS from the Homebrew Launcher.

No channels or stream URLs are included. Users provide their own playlist and
are responsible for having permission to access its streams.

## Controls

| Control | Action |
| --- | --- |
| Up / Down | Select a channel |
| Left / Right | Change channel-list page |
| A | Tune or retry |
| B | Cancel, return to the deck, or toggle the live channel drawer |
| L / R | Previous or next channel |
| Select | Cycle playback diagnostics |
| Start | Exit |

The launch scan adds compatible-looking stations as they are discovered. `*`
means the manifest advertises supported video metadata; `?` means final
compatibility cannot be known until tuning. `+` marks a station that played in
the current session and `!` marks a failed startup. Scanning pauses during
playback so it does not compete with the active stream.

## Playlist format

```m3u
#EXTM3U
#EXTINF:-1,Example Channel
https://example.test/live/index.m3u8
```

Extended-M3U `user-agent` and `referrer` attributes are supported for streams
that legitimately require them.

## Stream compatibility

The current player targets:

- live HLS with MPEG-TS segments;
- H.264/AVC video and AAC audio;
- unencrypted streams without byte ranges or fMP4 init maps;
- resolutions up to 640x480 and known frame rates up to 30.5 fps;
- individual compressed segments no larger than 4 MiB.

The scan is intentionally lightweight: it checks manifests without downloading
video or starting the decoder. Tuning performs the authoritative codec,
resolution, segment-size, and hardware checks. Mid-stream format changes are
handled with a bounded decoder relock or a safe return to the channel deck.

## How it works

```text
M3U playlist -> manifest scan -> channel deck
                                |
HLS playlist -> lowest compatible rendition
                                |
network producer -> bounded ring -> FFmpeg demux/AAC + MVD H.264 -> Citro3D
```

The network producer downloads new HLS segments while playback consumes the
ring. Startup depth and refill behavior adapt from delivery timing observed
during the current app session. Memory remains bounded, and changing channels
fully tears down the previous stream and decoder.

For technical detail, see [Architecture](docs/ARCHITECTURE.md) and
[Adaptive buffering](docs/ADAPTIVE_BUFFERING.md).

## Telemetry

Diagnostics are written to:

```text
sd:/3ds/retrotuner3ds/telemetry.csv
```

The previous launch is retained as `telemetry-prev.csv`. Logs contain pipeline,
buffer, timing, relock, underrun, and error data—never stream URLs, video, or
audio. Logging is capped at 512 KiB and playback continues if logging fails.

## Build and test

Install the devkitPro 3DS toolchain, then run:

```sh
make 3dsx -j4
./tests/run_live_host_tests.sh
```

Development and release details are in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).
See also [Contributing](CONTRIBUTING.md), [Changelog](CHANGELOG.md), and
[Roadmap](ROADMAP.md).

## Safety, credits, and license

RetroTuner3DS is a `.3dsx` homebrew application. It does not modify NAND,
firmware, boot configuration, Luma configuration, or the title database.

It is based on
[Video player for 3DS](https://github.com/Core-2-Extreme/Video_player_for_3DS)
by Core_2_Extreme and is distributed under **GPL-3.0-or-later**. See
[LICENSE](LICENSE), [Third-party notices](THIRD_PARTY_NOTICES.md), and
[dependency licenses](LICENSES/README.md).
