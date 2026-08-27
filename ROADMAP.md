# Roadmap

RetroTuner3DS already proves the main idea: a New 3DS can scan an M3U and play
the right kind of live HLS stream. The next job is making that experience less
fragile and easier for other people to understand.

## Now

- Keep testing low-bitrate channels for long playback, channel changes, and
  clean recovery after a broadcaster changes its live window.
- Track down freezes that happen even with a healthy network reserve. Those are
  usually decoder, timestamp, texture, or audio-path problems—not Wi-Fi.
- Finish the 1990s handheld-TV interface without covering useful diagnostics.
- Turn hardware reports into a small, reproducible compatibility guide.
- Prepare the first honest public pre-release with no bundled playlist.

## Later

- Favorites and channel groups.
- Optional refresh from a user-supplied playlist URL.
- Better support for safe HLS rendition and format changes.
- More graceful handling of audio-only and video-only failures.
- Evaluate other segment layouts only when their memory and teardown behavior
  can be bounded.

## Not planned

- On-console transcoding. The New 3DS cannot turn arbitrary HD video into a
  lightweight live stream in real time.
- DRM bypasses or bundled access to paid services.
- Pretending every URL in an M3U will work. The app will keep failing safely on
  streams outside its limits.
- Original 3DS/2DS support unless somebody proves a safe decoder path on that
  hardware.
