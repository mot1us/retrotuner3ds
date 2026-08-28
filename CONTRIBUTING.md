# Contributing

RetroTuner3DS is a strange little hardware project, so real New 3DS results are
just as valuable as code. Bug reports, stream compatibility notes, and focused
pull requests are all welcome.

## Code changes

1. Fork the repository and branch from `main`.
2. Keep the branch focused: `fix/rebuffer-loop`, `feature/channel-groups`, or
   something similarly clear.
3. Run `./tests/run_live_host_tests.sh`.
4. Build a `.3dsx` if console code changed.
5. Open a pull request and explain both the desktop and hardware results.

Commit subjects normally start with `fix:`, `feat:`, `docs:`, `test:`,
`build:`, or `refactor:`. This is a preference, not a reason to reject useful
work.

## Hardware reports

Please include what you know:

- console model;
- RetroTuner3DS version or commit;
- stream resolution, bitrate, codecs, and segment type;
- time to first frame and time before the problem appeared;
- whether audio, video, or both failed;
- whether the same stream currently works in VLC;
- telemetry or a Luma crash dump when available.

Do not post subscription URLs, credentials, access tokens, or private logs.
Public test streams are best when somebody else needs to reproduce the issue.

## Safety changes

The player rejects HLS layouts it cannot bound safely. Changes involving
encryption, byte ranges, fMP4, discontinuities, format changes, decoder reuse,
or memory limits need tests for the failure and cleanup paths—not only the happy
path.

## Before a release

- Host sanitizer tests pass.
- A clean devkitARM build succeeds.
- The candidate is tested on a real New 3DS.
- The version and changelog are current.
- Bundled dependency notices still match the shipped libraries.
- The `.3dsx` and release ZIP have SHA-256 checksums.
- No playlist, private media, credential-bearing URL, or generated build file is
  tracked.

## License

Contributions are distributed under the project's GPL-3.0-or-later license.
