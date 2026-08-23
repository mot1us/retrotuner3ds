# RetroTuner3DS

An experimental low-bitrate M3U/HLS stream player for the **New Nintendo 3DS**.
It has only been tested on that model; other 3DS-family systems are unverified.

> [!IMPORTANT]
> RetroTuner3DS is early homebrew. It works best with low-bitrate H.264/AAC streams
> at 480p or below. Streams can change or disappear, and not every channel will
> be compatible.

## What works

- Live, continuous HLS playback—segments are streamed into a bounded memory
  ring rather than downloading an entire program first.
- Hardware-accelerated H.264 decoding on the tested New Nintendo 3DS.
- Up to 32 channels from a user-provided `channels.m3u` file, shown in pages.
- Automatic selection of the lowest advertised HLS rendition.
- Retro dual-screen channel deck, buffering state, and stream diagnostics.
- Full teardown of the stream and decoder when leaving a channel.

The most reliable hardware-tested signals are **360p-class H.264/AAC streams
below roughly 1 Mbps**. Live mode now fails closed above 640x480 or a known
30 fps limit; 720p and larger sources are rejected before hardware decoding.

## Install

1. Use a New Nintendo 3DS with the Homebrew Launcher.
2. Copy `retrotuner3ds.3dsx` to
   `sd:/3ds/retrotuner3ds/retrotuner3ds.3dsx`.
3. Copy your playlist to `sd:/3ds/retrotuner3ds/channels.m3u`.
4. Start RetroTuner3DS from the Homebrew Launcher.

RetroTuner3DS includes no channels or stream URLs. Users provide their own M3U
playlist and are responsible for having permission to access its streams.

## Controls

| Control | Action |
| --- | --- |
| D-pad Up/Down | Choose a channel |
| A | Tune the selected channel |
| B | Cancel tuning, or stop playback and return to the channel deck |
| L / R | Queue the previous or next channel while tuning or playing |
| Select | Toggle playback details |
| Start | Exit |

After a no-signal or player error, use `A` to retry, `B` to return to the
channel deck, or `L`/`R` to try the adjacent signal.

## Playlist format

RetroTuner3DS supports a deliberately small subset of extended M3U:

```m3u
#EXTM3U
#EXTINF:-1,Example Channel
https://example.test/live/index.m3u8
```

The parser accepts up to 32 channels. `user-agent` and `referrer` attributes
are supported for streams that legitimately require them. If
`sd:/3ds/retrotuner3ds/channels.m3u` is missing or invalid, the app displays its
expected location instead of loading bundled stations.

## Stream compatibility

RetroTuner3DS currently targets:

- live HLS using MPEG-TS segments;
- H.264/AVC video and AAC audio;
- unencrypted streams without byte ranges, discontinuities, or fMP4 init maps;
- New 3DS hardware decoding;
- low-resolution, low-bitrate variants.

For console safety, the current live path also rejects video above 640x480,
known frame rates above 30.5 fps, partial segments, and mid-stream HLS
discontinuities. A rejected channel returns to the deck instead of falling back
to software decoding.

At tuning time, the player selects the lowest rendition advertised by the
channel and rejects unsupported HLS layouts. Initial tuning has a 30-second
total deadline and remains cancelable, so a stalled URL cannot trap the channel
deck. This compatibility filtering does not guarantee that every URL in a
playlist will play.

It does **not** transcode video. The 3DS can scale a decoded frame for its
screen, but scaling does not reduce the work required to decode a 720p or
1080p source. A channel must already publish a rendition the console can keep
up with.

## How it works

```text
M3U channel list
      ↓
HLS master/media playlist → lowest compatible rendition
      ↓
network producer → 6 MiB bounded ring buffer
      ↓
FFmpeg demux + AAC audio + New 3DS MVD H.264 decoder
      ↓
Citro3D video output
```

The producer keeps downloading new HLS segments while the player consumes the
ring. A bounded refill after a true underrun provides basic rebuffering without allowing the
stream to consume memory indefinitely.

For a deeper tour, see [Architecture](docs/ARCHITECTURE.md).

## Build and test

The repository preserves the upstream project's vendored 3DS libraries. Install
the devkitPro 3DS toolchain, then run:

```sh
export DEVKITPRO=/opt/devkitpro
export DEVKITARM="$DEVKITPRO/devkitARM"
export PATH="$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH"

make 3dsx -j4
```

The output is `retrotuner3ds.3dsx`.

The portable HLS and H.264 helpers also have sanitizer-backed host tests:

```sh
./tests/run_live_host_tests.sh
```

## Development

- `main` contains the latest hardware-tested project state.
- Work happens on short-lived `feature/*`, `fix/*`, or `docs/*` branches.
- Pull requests should keep changes focused and record the hardware test result.
- Releases are tagged with semantic versions such as `v0.5.0`.

See the [Development guide](docs/DEVELOPMENT.md),
[Contributing](CONTRIBUTING.md), [Changelog](CHANGELOG.md), and the
[Roadmap](ROADMAP.md).

## Safety

RetroTuner3DS is distributed as a `.3dsx` homebrew application. It does not modify
NAND, firmware, boot configuration, Luma configuration, or the title database.
Its reversible settings/log files live under `sd:/3ds/retrotuner3ds/`.

## Credits and license

RetroTuner3DS is based on
[Video player for 3DS](https://github.com/Core-2-Extreme/Video_player_for_3DS)
by Core_2_Extreme. This modified version is licensed under
**GPL-3.0-or-later**. See [LICENSE](LICENSE) and
[Third-party notices](THIRD_PARTY_NOTICES.md). Exact source revisions and
retained dependency licenses are listed in the [LICENSES](LICENSES/README.md)
directory.
