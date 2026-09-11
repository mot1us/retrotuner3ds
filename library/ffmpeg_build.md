# Rebuilding FFmpeg for RetroTuner3DS

Normal app builds use the archives and headers already in `library/`. Rebuild
FFmpeg only when changing the dependency, not for each app release.

RetroTuner3DS uses [Core-2-Extreme's FFmpeg for 3DS](https://github.com/Core-2-Extreme/FFmpeg_for_3DS),
with a smaller configuration than the upstream video player:

- AAC and H.264 decoders;
- Matroska, MOV/MP4, and MPEG-TS demuxers;
- the file protocol and pthread support;
- no encoders, muxers, or x264/LAME/dav1d integration.

HLS downloads are handled by our network code. FFmpeg consumes MPEG-TS through
the app's custom input callback, so neither FFmpeg's HLS demuxer nor its HTTP
protocol is required. MPEG-TS support must remain enabled.

## Recorded build

The pinned source is
[`dab24a843203b2b191f40e39907fb146f688ec5c`](https://github.com/Core-2-Extreme/FFmpeg_for_3DS/commit/dab24a843203b2b191f40e39907fb146f688ec5c).
The bundled `libavutil/ffversion.h` identifies `git-2026-07-10-dab24a8432`, and
the archive configuration strings record the options below. Object compiler
metadata reports devkitARM GCC 16.1.0. The locally installed devkitARM package
is `r68-1`.

This reconstructs the recorded source/configuration; it is not a verified
byte-for-byte recipe. The original build environment and any unrecorded source
patches are not captured by an archive's version string. A future dependency
update should retain the source tree, patches, tool/package versions,
`config.h`, and `ffbuild/config.log` with its corresponding-source archive.

## Configure and build

These commands expect devkitPro at `/opt/devkitpro`, including libctru headers
and libraries. Use a new working directory; do not overwrite an existing
FFmpeg checkout or the bundled libraries while experimenting.

```sh
git clone --branch 3ds --no-checkout \
    https://github.com/Core-2-Extreme/FFmpeg_for_3DS.git FFmpeg_for_3DS-rebuild
cd FFmpeg_for_3DS-rebuild
git switch --detach dab24a843203b2b191f40e39907fb146f688ec5c

ffmpeg_stage="$PWD/../ffmpeg-3ds-stage"
./configure \
    --enable-cross-compile \
    --cross-prefix=/opt/devkitpro/devkitARM/bin/arm-none-eabi- \
    --prefix="$ffmpeg_stage" \
    --cpu=armv6k --arch=arm --target-os=linux \
    --extra-cflags="-mfloat-abi=hard -mtune=mpcore -mtp=cp15 -Wno-error=incompatible-pointer-types -I/opt/devkitpro/libctru/include" \
    --extra-ldflags="-mfloat-abi=hard -L/opt/devkitpro/libctru/lib -specs=3dsx.specs" \
    --extra-libs=-lctru \
    --enable-optimizations \
    --disable-filters --disable-devices --disable-bsfs --disable-parsers \
    --disable-hwaccels --disable-debug --disable-stripping --disable-programs \
    --disable-avdevice --disable-avfilter --disable-decoders --disable-demuxers \
    --disable-encoders --disable-muxers --disable-asm --disable-protocols \
    --disable-txtpages --disable-podpages --disable-manpages --disable-htmlpages \
    --disable-doc --enable-inline-asm --enable-vfp --enable-armv5te --enable-armv6 \
    --enable-decoder=aac,h264 \
    --enable-demuxer=matroska,mov,mpegts \
    --enable-protocol=file --enable-pthreads
make -j4
make install
```

The staging prefix replaces the original machine-specific output path; the
other options match the configuration embedded in the checked-in archives.
No system-wide install or `sudo` is needed.

## Before replacing the bundled files

Review the staged `libavcodec`, `libavformat`, `libavutil`, `libswresample`, and
`libswscale` archives together with their matching header directories. Keep
unrelated libraries untouched. Useful checks from the project root:

```sh
strings library/lib/libavcodec.a | rg -- '--enable-cross-compile|libavcodec license'
strings library/lib/libavformat.a | rg -- '--enable-cross-compile|libavformat license'
cat library/include/libavutil/ffversion.h
```

After an intentional replacement, run the host tests, make a clean `.3dsx`
build, and test the exact artifact on a New 3DS. A successful rebuild alone
does not verify AAC timing, MVD playback, or channel switching. Record the
dependency change and update [the source/license inventory](../LICENSES/README.md).

The bundled FFmpeg archives report `LGPL version 2.1 or later` and were built
without `--enable-gpl`. This differs from the broader upstream build recipe
previously stored here. The app remains GPL-3.0-or-later; retain the existing
upstream notices and source license files when preparing a release.
