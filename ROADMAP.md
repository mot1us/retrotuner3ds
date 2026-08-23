# Roadmap

RetroTuner3DS is a working experiment, not a finished media platform. Near-term work
is intentionally focused on reliability and clarity.

## Next

- Finish hardware testing and polish for the Pixel Deck UI.
- Improve rebuffer timing after long playback sessions.
- Add a small compatibility database based on reproducible hardware reports.
- Make channel errors understandable without exposing the inherited debug UI.
- Add release packaging and checksum automation.

## Later

- Channel groups and favorites.
- Optional playlist refresh from a user-supplied URL.
- Better handling of HLS variant changes.
- Evaluate safe support for additional segment/container layouts.
- Explore audio/video clock behavior on marginal streams.

## Not planned

- On-console transcoding. The New 3DS does not have enough general-purpose
  headroom to turn arbitrary HD streams into lower-resolution live video.
- DRM circumvention or bundled access to paid services.
- Support for original 3DS/2DS hardware decoding.
