# Buffering

Buffering on the New 3DS is a balancing act. We want enough live data ready to
ride out Wi-Fi hiccups, but we cannot keep downloading forever or hand partial
segments to the decoder.

## The 6 MiB ring

The player has one fixed 6 MiB ring for compressed HLS data. The network thread
writes complete MPEG-TS segments into it and FFmpeg reads them from the other
side.

Six MiB is the maximum capacity, not a promise that six MiB will always be
available. At the live edge, the producer sometimes has to wait for the station
to publish its next segment. Making the ring larger would not create data that
does not exist yet.

The producer also pauses near the high-water mark so it cannot overwrite unread
data. None of this memory grows with viewing time.

## Why a full reserve can still freeze

The reserve meter only describes compressed network data waiting for FFmpeg.
It does not prove that video frames are leaving MVD or that audio buffers are
reaching the DSP.

A channel can therefore show a healthy green reserve while the picture is
frozen because of a decoder, timestamp, texture, or format problem. Telemetry
keeps network, video, and audio counters separate for exactly this reason.

## Starting a channel

A first tune is treated as cold. It normally starts two published segments
behind the newest one and stages two complete segments before handing the
stream to FFmpeg. That costs a little startup time but gives the decoder a real
reserve instead of immediately chasing the broadcaster.

The app remembers up to 32 small channel profiles for the current session. A
profile stores only a hash and numeric measurements—never the URL. After enough
good samples, tuning that channel again can use:

- one segment for a proven healthy feed;
- two segments for a cold or ordinary feed;
- up to three segments of live-edge lag for a marginal feed.

The profile disappears when the app closes.

## What gets measured

After each complete segment, the controller updates integer moving averages for:

```text
content rate  = segment bytes / media duration
network rate  = segment bytes / download time
headroom      = media duration / download time
delivery gap  = time between completed segments
jitter        = variation in that delivery gap
```

It also tracks real empty-ring underruns, time spent stalled, largest segment,
playlist polls that found nothing new, and the lowest observed reserve.

Headroom is the easiest number to read. `2.0x` means that sample downloaded
twice as fast as playback consumes it. Values near or below `1.0x` mean the
channel cannot reliably keep up at that moment.

## Recovering from an underrun

The player only enters refill mode after the compressed ring actually reaches
zero. It does not pause a working stream just because the meter looks low.

An isolated underrun waits for one complete segment. If another happens within
90 seconds, the player asks for a deeper target based on two recent segments or
the controller's recommendation. That target is capped at 3 MiB, and the wait
is bounded between 2.5 and 8 seconds. If the broadcaster has not published
enough data by then, playback resumes with whatever complete data is safely
available.

After a discontinuity or other clean decoder relock, the player always rebuilds
a conservative two-segment reserve. It does not trust the warm profile from the
old decoder session.

## What this is not

This is adaptive buffering, not adaptive-bitrate playback. RetroTuner3DS can
choose the lowest rendition advertised when it first tunes, but it does not
switch renditions while MVD is active.

It also does not transcode. If a station only offers a heavy 720p or 1080p
feed, changing the buffer cannot make that video cheap enough for the New 3DS.

## Limits we do not move

- The ring stays at 6 MiB.
- An individual video or combined A/V segment stays capped at 4 MiB.
- Only complete, validated MPEG-TS segments are published to FFmpeg.
- Encryption, byte ranges, fMP4, cancellation, and format-boundary checks stay
  in place.
- There is still only one producer, one FFmpeg consumer, and one MVD session.

The goal is smoother playback inside known-safe limits, not getting every HLS
channel on the internet to play.
