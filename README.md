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
- An incremental "airwave" scan of up to 64 user-provided channels. Stations
  appear as their small HLS manifests pass compatibility checks.
- Automatic selection of the lowest advertised HLS rendition.
- Retro dual-screen channel deck, buffering state, and stream diagnostics.
- A bounded diagnostics log for hardware buffer testing.
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

At launch, RetroTuner3DS starts with an empty deck and checks channels in M3U
order. `*` means the master playlist explicitly advertises a supported size
and frame rate; `?` means the HLS layout passed but the server did not publish
enough metadata to know until tuning. You may tune any discovered station
without waiting for the scan to finish. Discovery pauses completely during
playback and resumes after returning to the deck, so it cannot compete with
the live stream for the New 3DS Wi-Fi connection.

## Hardware telemetry

Each app launch writes:

    sd:/3ds/retrotuner3ds/telemetry.csv

The prior launch is retained as `telemetry-prev.csv`, so an accidental relaunch
does not immediately erase the hardware run we need to inspect.

During scanning, tuning, and playback, the file records monotonic elapsed time, channel
name, pipeline state, compressed-ring depth, segment delivery timing, shadow
buffer recommendations, applied startup lag, warm/cold profile state,
live-window sequence resynchronizations, underruns, and errors. Normal
channel-switch cancellation is not recorded as an error. Isolated oversized
segment skips are counted separately. It never records
stream URLs, video, or audio. Rows are buffered in
ordinary RAM and sampled every two seconds for the first minute after launch
or a channel change, then every ten seconds. State changes, errors, underruns,
and channel changes are recorded immediately. Data is flushed every ten
seconds and on important events, and capped at 512 KiB. If logging cannot be opened,
playback continues normally and the channel deck reports LOG OFF.

During one app session, channels with at least three validated segment samples
receive a small in-memory buffering profile. A cold channel begins two segments
behind the live edge; later tunes use one segment for healthy delivery, two for
marginal delivery, or three for unsustainable/repeatedly underrunning delivery.
Cold tunes stage two complete initial segments to protect decoder startup from
draining the reserve. A proven warm profile with healthy delivery and no
underruns uses the faster one-segment path. Neither path increases the 6 MiB
ring or 4 MiB atomic segment limit.

## Playlist format

RetroTuner3DS supports a deliberately small subset of extended M3U:

```m3u
#EXTM3U
#EXTINF:-1,Example Channel
https://example.test/live/index.m3u8
```

The parser accepts up to 64 channels. `user-agent` and `referrer` attributes
are supported for streams that legitimately require them. If
`sd:/3ds/retrotuner3ds/channels.m3u` is missing or invalid, the app displays its
expected location instead of loading bundled stations.

## Stream compatibility

RetroTuner3DS currently targets:

- live HLS using MPEG-TS segments;
- H.264/AVC video and AAC audio, either multiplexed together or published as
  aligned MPEG-TS HLS audio/video renditions;
- unencrypted streams without byte ranges or fMP4 init maps;
- New 3DS hardware decoding;
- low-resolution, low-bitrate variants.

For console safety, the current live path also rejects video above 640x480,
known frame rates above 30.5 fps and partial segments. A mid-stream HLS
discontinuity or detected format boundary first tears down the decoder, then
attempts a bounded clean relock; changed media is never fed into the active MVD
session. Relocks rebuild a conservative two-segment reserve and record their
boundary reason in `telemetry.csv`. A rejected channel returns to the deck instead of falling back to
software decoding.

At tuning time, the player selects the lowest rendition advertised by the
channel and rejects unsupported HLS layouts. Initial tuning has a 30-second
total deadline and remains cancelable, so a stalled URL cannot trap the channel
deck. This compatibility filtering does not guarantee that every URL in a
playlist will play.

The launch scan is deliberately cheaper than a full tune. It fetches only the
root playlist and, when needed, its selected media playlist with a short
timeout. It never downloads a video segment or initializes FFmpeg/MVD. Known
sources above 640x480 or 30.5 fps, encrypted HLS, byte-range/fMP4 layouts, VOD
playlists, malformed manifests, and offline URLs are left out of the deck.
Direct media playlists often omit resolution metadata, so `?` stations still
receive the authoritative codec/resolution and segment-size checks when tuned.

It does **not** transcode video. The 3DS can scale a decoded frame for its
screen, but scaling does not reduce the work required to decode a 720p or
1080p source. A channel must already publish a rendition the console can keep
up with.

## How it works

```text
M3U source list → manifest-only airwave scan → discovered station deck
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
[Adaptive buffering plan](docs/ADAPTIVE_BUFFERING.md),
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
