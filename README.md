# RetroTuner3DS

RetroTuner3DS plays low-bitrate HLS streams from an extended M3U playlist on the
New Nintendo 3DS. It has only been tested on a New Nintendo 3DS and is still
experimental: some streams will fail even after passing the opening scan.

## What works best

Use H.264 video with AAC audio in MPEG-TS HLS segments, ideally 360p at roughly
1 Mbps or less. The app prefers the lowest-bitrate compatible rendition and
does not transcode. The upper limit is 640x480 at a known 30.5 fps or lower;
encrypted streams, byte ranges, and fMP4 init maps are unsupported.

The opening scan checks manifests, so its results are advisory. A `*` means a
channel advertises compatible video details; `?` means it needs a playback
check. After tuning, `+` means it played and `!` means it failed to start in
this session. You can always retry.

## Install

1. Put `retrotuner3ds.3dsx` at
   `sd:/3ds/retrotuner3ds/retrotuner3ds.3dsx`.
2. Put your playlist at `sd:/3ds/retrotuner3ds/channels.m3u`.
3. Open RetroTuner3DS from the Homebrew Launcher.

No channels or stream URLs are included. Supply your own playlist with up to
64 channels and a maximum size of 128 KiB. Each HTTP(S) URL needs a preceding
`#EXTINF` entry. Extended-M3U `user-agent` and `referrer` attributes are supported.
Use streams you have permission to access.

## Controls

| Control | Action |
| --- | --- |
| Up / Down | Pick a channel |
| Left / Right | Change channel-list page |
| A | Tune or retry; play/pause while watching |
| B | Cancel or go back; open/close the channel drawer while video keeps playing |
| L / R | Previous or next channel |
| Select | Cycle the diagnostic screens |
| X | Open the theme picker |
| Start | Exit |

## Themes

Press X to choose Classic, Cyberpunk, Wasteland, or Old-Time Radio. Use the
D-pad to preview, A to apply and save for the next launch, or B to cancel.
Video keeps playing while you choose, and its colors stay the same.

## Troubleshooting

Diagnostic logs live in `sd:/3ds/retrotuner3ds/`: `telemetry.csv` for
the current run, `telemetry-prev.csv` for the previous run, and `theme.cfg.log`
for the last theme-save attempt. Playback logs stop at 512 KiB and contain no
stream URLs, video, or audio. Logs are not required for playback.

## Building it

Install the devkitPro 3DS toolchain, then run:

```sh
./tests/run_live_host_tests.sh
make 3dsx -j4
```

See the [development guide](docs/DEVELOPMENT.md) for setup and release steps,
[Architecture](docs/ARCHITECTURE.md) and [Buffering](docs/ADAPTIVE_BUFFERING.md)
for technical details, and [Contributing](CONTRIBUTING.md),
[Changelog](CHANGELOG.md), and [Roadmap](ROADMAP.md) for project information.

## Safety, credits, and license

RetroTuner3DS is a `.3dsx` homebrew app. It does not touch NAND, firmware,
boot configuration, Luma configuration, or the title database.

The project is built on
[Video player for 3DS](https://github.com/Core-2-Extreme/Video_player_for_3DS)
by Core_2_Extreme and is released under **GPL-3.0-or-later**. See
[LICENSE](LICENSE), [Third-party notices](THIRD_PARTY_NOTICES.md), and the
[bundled dependency licenses](LICENSES/README.md).
