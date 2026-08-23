# Contributing to RetroTuner3DS

Thanks for helping make live video on a tiny old handheld a little less
impossible.

## Workflow

1. Fork the repository and branch from `main`.
2. Use a short branch name such as `fix/rebuffer-loop`,
   `feature/channel-groups`, or `docs/build-notes`.
3. Keep commits focused. Preferred subjects are `fix: ...`, `feat: ...`,
   `docs: ...`, `test: ...`, `build: ...`, or `refactor: ...`.
4. Run `./tests/run_live_host_tests.sh`.
5. Build a `.3dsx` when the change touches console code.
6. Open a pull request and include the hardware and stream test result.

Direct commits to `main` are reserved for repository maintenance. Normal code
changes should arrive through a pull request, even for maintainers.

## Hardware reports

Useful reports include:

- exact console model and whether it is a New 3DS-family system;
- RetroTuner3DS version or commit;
- stream resolution, bitrate, video/audio codecs, and segment format;
- approximate time before the problem appeared;
- buffer, download, underrun, and error values shown by diagnostics;
- whether the same URL currently plays in VLC.

Do not post private credentials, subscription URLs, access tokens, or logs that
contain them. Prefer a public test stream when reproducing a bug.

## Compatibility changes

The player intentionally rejects HLS features it cannot safely bound yet.
Changes involving encryption, byte ranges, fMP4, discontinuities, redirects,
or memory limits need tests for both success and cleanup paths.

## Release checklist

- Host sanitizer tests pass.
- A clean devkitARM build succeeds.
- The release candidate is tested on real New 3DS hardware.
- `CHANGELOG.md` and the version string are updated.
- The `.3dsx` checksum is recorded.
- The release is tagged `vMAJOR.MINOR.PATCH` and uploaded as a GitHub Release.

## License

By contributing, you agree that your contribution is distributed under the
project's GPL-3.0-or-later license.
