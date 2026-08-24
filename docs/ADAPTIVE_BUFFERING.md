# Adaptive buffering plan

## Why the current meter can stay low

The 6 MiB ring is a capacity limit, not a promise that six MiB of live data
exists. The producer already downloads every published segment until that ring
approaches 5 MiB. At the live edge it must wait for the broadcaster to publish
the next segment, so increasing the allocation alone cannot create more
reserve.

The current three-second target applies only after a real empty-ring underrun.
It does not hold playback at three seconds during normal operation. That is
intentional: proactively pausing a healthy low reserve would manufacture a
visible stall.

## Measurements

The controller will update these integer EWMAs after each complete segment:

```text
content bitrate = segment bytes * 8000 / media duration ms
network bitrate = segment bytes * 8000 / download time ms
headroom        = media duration ms / download time ms
commit gap      = this commit time - previous commit time
```

It will also track commit-gap deviation, recent underruns, total stall time,
largest segment, media-playlist polls with no new segment, and ring minima.
Network reserve and decoder stalls remain separate signals.

## Rollout

### rc9.7: shadow controller

- Calculate `COLD`, `HEALTHY`, `AT RISK`, `MARGINAL`, `REFILL`, and
  `UNSUSTAINABLE` states in a fixed-size, integer-only helper without changing
  playback.
- Display headroom, delivery-gap jitter, desired reserve, and recommended
  live-edge lag on a clearly labelled `SHADOW` page.
- Compare the recommendations with five-minute hardware tests.

The shadow helper has no clock, lock, allocator, network access, or decoder
dependency. The live-stream owner supplies timestamps and validated segment
samples while holding its existing lock. Failed or partial downloads are never
sampled, and time spent deliberately paused at the ring high-water mark is
excluded from the following delivery-gap sample.

`SELECT` cycles the shadow page, existing pipeline diagnostics, and a clean
view. A shadow recommendation is telemetry only in rc9.7: it does not alter the
one-segment initial tune, three-second refill, 5 MiB high-water mark, 6 MiB
ring, or 4 MiB atomic segment cap.

### rc9.8: adaptive start depth

- Keep downloading exactly one complete initial segment.
- Start one published segment behind the newest for healthy channels, two for
  cold or marginal channels, and three after repeated underruns.
- Never select across an HLS discontinuity.

Starting farther behind adds broadcast latency rather than download work. It
also gives the producer already-published segments to fetch while FFmpeg and
MVD initialize, which is the best available way to build reserve entirely on
the New 3DS.

### rc9.9: adaptive recovery

- Resume after one complete segment for an isolated underrun.
- After another underrun within 90 seconds, try to collect two segments.
- If the second segment has not been published, resume with one after a bounded
  wait of at most the target duration plus two seconds, capped at eight seconds.

The desired reserve will be derived from segment duration, commit-gap jitter,
download headroom, and recent underruns, then clamped to 2.5--12 seconds and
128 KiB--3 MiB.

## Session profiles

A fixed 32-entry table will retain measurements and a recommended lag for each
channel during the current app session. It requires only a few KiB of ordinary
RAM, writes nothing to the SD card, and never logs channel URLs. Persistence can
be considered only after the recommendations prove useful on hardware.

## Safety invariants

- Keep the 6 MiB ordinary-RAM ring and 4 MiB atomic segment staging limit.
- Publish only complete, validated MPEG-TS segments.
- Preserve sequence, discontinuity, cancellation, and tune-timeout checks.
- Keep one producer, one FFmpeg consumer, one MVD instance, and serialized
  teardown.
- Never switch rendition while MVD is active; this is buffering adaptation,
  not transcoding or adaptive-bitrate playback.
- Keep `B`, `L`, and `R` responsive during tuning and refill waits.

## Acceptance targets

- No regression on proven low-bitrate channels.
- At least 50% fewer visible stalls on marginal channels in five-minute tests.
- No additional linear-memory allocation.
- Bounded startup/recovery waits and no crash across ten rapid channel changes.
